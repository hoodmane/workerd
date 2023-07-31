// Copyright (c) 2017-2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include <workerd/api/worker-rpc.h>
#include <workerd/api/global-scope.h>

namespace workerd::api {

kj::Promise<WorkerRpc::ResponseAndResult> WorkerRpc::sendWorkerRpc(
    jsg::Lock& js,
    kj::StringPtr name,
    const v8::FunctionCallbackInfo<v8::Value>& args) {

  auto& ioContext = IoContext::current();
  auto worker = getClient(ioContext, kj::none, "getJsRpcTarget"_kjc);
  auto event = kj::heap<api::GetJsRpcTargetCustomEventImpl>(WORKER_RPC_EVENT_TYPE);

  rpc::JsRpcTarget::Client client = event->getCap();
  auto builder = client.callRequest();
  builder.setMethodName(name);

  kj::Vector<jsg::JsValue> argv(args.Length());
  for (int n = 0; n < args.Length(); n++) {
    argv.add(jsg::JsValue(args[n]));
  }

  // If we have arguments, serialize them.
  if (argv.size() > 0) {
    auto ser = jsg::serializeV8Rpc(js, js.arr(argv.asPtr()));
    builder.initSerializedArgs().setV8Serialized(kj::mv(ser));
  }

  auto callResult = builder.send();
  auto customEventResult = worker->customEvent(kj::mv(event));

  auto resp = co_await callResult;
  auto result = co_await customEventResult;
  co_return WorkerRpc::ResponseAndResult {
      .rpcResponse = kj::mv(resp),
      .customEventResult = kj::mv(result) };
}


jsg::Value WorkerRpc::handleRpcResponse(jsg::Lock& js, WorkerRpc::ResponseAndResult outcome) {
  // Both promises are ready!
  auto& rpcResult = outcome.rpcResponse;
  auto& customEvent = outcome.customEventResult;

  auto serializedResult = rpcResult.getResult().getV8Serialized();
  auto deserialized = jsg::deserializeV8Rpc(js, kj::heapArray(serializedResult.asBytes()));
  KJ_REQUIRE(customEvent.outcome == EventOutcome::OK, "got unexpected event outcome for WorkerRpc");

  // TODO(now): Need to think about how we handle returning vs. throwing error from remote.
  if (deserialized.isNativeError()) {
    JSG_FAIL_REQUIRE(Error, deserialized.toString(js));
  } else {
    return jsg::Value(js.v8Isolate, kj::mv(deserialized));
  }
}

kj::Maybe<jsg::JsValue> WorkerRpc::getNamed(jsg::Lock& js, kj::StringPtr name) {
  // Named intercept is enabled, this means we won't default to legacy behavior.
  // The return value of the function is a promise that resolves once the remote returns the result
  // of the RPC call.
  return jsg::JsValue(js.wrapReturningFunction(js.v8Context(), [this, methodName=kj::str(name)]
      (jsg::Lock& js, const v8::FunctionCallbackInfo<v8::Value>& args) {
        auto& ioContext = IoContext::current();
        // Wait for the RPC to resolve and then process the result.
        return js.wrapSimplePromise(ioContext.awaitIo(js, sendWorkerRpc(js, methodName, args),
            [this] (jsg::Lock& js, WorkerRpc::ResponseAndResult result) -> jsg::Value {
          return handleRpcResponse(js, kj::mv(result));
        }));
      }
  ));
}

// The capability that lets us call remote methods over RPC.
// The client capability is dropped after each callRequest().
class JsRpcTargetImpl final : public rpc::JsRpcTarget::Server {
public:
  JsRpcTargetImpl(
      kj::Own<kj::PromiseFulfiller<void>> callFulfiller,
      IoContext& ctx,
      kj::Maybe<kj::StringPtr> entrypointName)
      : callFulfiller(kj::mv(callFulfiller)), ctx(ctx), entrypointName(entrypointName) {}

  // Handles the delivery of JS RPC method calls.
  kj::Promise<void> call(CallContext callContext) override {
    auto methodName = kj::heapString(callContext.getParams().getMethodName());
    auto serializedArgs = callContext.getParams().getSerializedArgs().getV8Serialized().asBytes();

    // Try to execute the requested method.
    try {
      co_await ctx.run(
          [this,
          methodName=kj::mv(methodName),
          serializedArgs = kj::mv(serializedArgs),
          entrypointName = entrypointName,
          &callContext] (Worker::Lock& lock) -> kj::Promise<void> {

        auto reader = callContext.initResults(capnp::MessageSize { 4, 1 }).initResult();

        auto& handler = KJ_REQUIRE_NONNULL(lock.getExportedHandler(entrypointName, ctx.getActor()),
            "Failed to get handler to worker.");

        jsg::Lock& js = lock;
        auto handle = handler.self.getHandle(lock);
        auto methodStr = jsg::v8StrIntern(lock.getIsolate(), methodName);
        auto fnHandle = jsg::check(handle->Get(lock.getContext(), methodStr));
        if (!(fnHandle->IsFunction() && !fnHandle->IsPrivate())) {
          auto errString = "The RPC receiver does not implement the requested method."_kj;
          reader.setV8Serialized(jsg::serializeV8Rpc(js, js.error(errString)));
        } else {
          auto fn = fnHandle.As<v8::Function>();
          // TODO(now): How do we differentiate throwing an error from JS vs. returning one?
          auto result = [&]() {
            // We received arguments from the client, deserialize them back to JS.
            if (serializedArgs.size() > 0) {
              auto args = KJ_REQUIRE_NONNULL(
                  jsg::deserializeV8Rpc(js, kj::heapArray(serializedArgs)).tryCast<jsg::JsArray>(),
                  "expected JsArray when deserializing arguments.");
              // Call() expects a `Local<Value> []`... so we populate an array.
              auto arguments = kj::heapArray<v8::Local<v8::Value>>(args.size());
              for (size_t i = 0; i < args.size(); ++i) {
                arguments[i] = args.get(js, i);
              }
              return jsg::check(fn->Call(lock.getContext(), fn, args.size(), arguments.begin()));
            } else {
              return jsg::check(fn->Call(lock.getContext(), fn, 0, nullptr));
            }
          }();

          if (fn->IsAsyncFunction() && result->IsPromise()) {
            // The method was async and we have a promise, so we need to wait for the promise
            // to resolve before setting the response.
            auto jsProm = js.toPromise(result.As<v8::Promise>());

            // We can't co_await a jsg::Promise, so we will use a promise-fulfiller pair instead.
            auto [promise, fulfiller] = kj::newPromiseAndFulfiller<void>();
            jsProm.then(js, [&reader, fulfiller=kj::mv(fulfiller)]
                (jsg::Lock& lock, jsg::Value value) mutable {
              auto jsVal = jsg::JsValue(value.getHandle(lock));
              reader.setV8Serialized(jsg::serializeV8Rpc(lock, kj::mv(jsVal)));
              fulfiller->fulfill();
            });
            // Wait on the kj::Promise, which will resolve once the JS promise has resolved.
            co_await promise;
          } else {
            // We already have the result, set the rpc call response.
            auto jsValue = jsg::JsValue(result);
            reader.setV8Serialized(jsg::serializeV8Rpc(js, kj::mv(jsValue)));
          }
        }
      });
    } catch(kj::Exception e) {
      if (auto desc = e.getDescription();
          !jsg::isTunneledException(desc) && !jsg::isDoNotLogException(desc)) {
        LOG_EXCEPTION("JsRpcTargetCall"_kj, e);
      }
    }
    // Upon returning, we want to fulfill the callPromise so customEvent can continue executing.
    KJ_DEFER(callFulfiller->fulfill(););
    co_return;

  }

  KJ_DISALLOW_COPY_AND_MOVE(JsRpcTargetImpl);

private:
  kj::Own<kj::PromiseFulfiller<void>> callFulfiller;
  IoContext& ctx;
  kj::Maybe<kj::StringPtr> entrypointName;
};

GetJsRpcTargetEvent::GetJsRpcTargetEvent()
    : ExtendableEvent("getJsRpcTarget") {};

kj::Promise<WorkerInterface::CustomEvent::Result> GetJsRpcTargetCustomEventImpl::run(
    kj::Own<IoContext::IncomingRequest> incomingRequest,
    kj::Maybe<kj::StringPtr> entrypointName) {
  incomingRequest->delivered();
  auto [callPromise, callFulfiller] = kj::newPromiseAndFulfiller<void>();
  capFulfiller->fulfill(kj::heap<JsRpcTargetImpl>(
      kj::mv(callFulfiller), incomingRequest->getContext(), entrypointName));

  // `callPromise` resolves once `JsRpcTargetImpl::call()` (invoked by client) completes.
  co_await callPromise;
  co_await incomingRequest->drain();

  co_return WorkerInterface::CustomEvent::Result {
    .outcome = EventOutcome::OK
  };
}

kj::Promise<WorkerInterface::CustomEvent::Result>
  GetJsRpcTargetCustomEventImpl::sendRpc(
    capnp::HttpOverCapnpFactory& httpOverCapnpFactory,
    capnp::ByteStreamFactory& byteStreamFactory,
    kj::TaskSet& waitUntilTasks,
    rpc::EventDispatcher::Client dispatcher) {
  auto req = dispatcher.getJsRpcTargetRequest();
  this->capFulfiller->fulfill(req.send().getServer());
  return WorkerInterface::CustomEvent::Result {
    .outcome = EventOutcome::OK
  };
}
}; // namespace workerd::api
