import bcrypt
from js import Response


salt = b"$2b$12$Wh/sgyuhro5ofqy2.5znc.35AjHwTTZzabz.uUOya8ChDpdwvROnm"
def fetch(request):
  return Response.new(bcrypt.hashpw(b"hello world", salt))
