import socket, threading, hashlib, struct #, time
from os import mkdir

port = 7850
serversocket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
fcid0dict = {}

# basically stolen from kurisu
def verify_fc(fc):
    if fc > 0x7FFFFFFFFF:
        return None
    principal_id = fc & 0xFFFFFFFF
    checksum = (fc & 0xFF00000000) >> 32
    return hashlib.sha1(struct.pack('<L', principal_id)).digest()[0] >> 1 == checksum

def readd(clientsocket):
  print("Hello from the readd thread!")
  try:
    mkdir("part1s")
  except:
    pass
  while True:
    #print("In read loop")
    fc = int.from_bytes(clientsocket.recv(8), 'little')
    lfcs = clientsocket.recv(8)
    #print("past recvs with fc of %d" % fc)
    id0 = fcid0dict[fc]
    print("\nGot LFCS for %d %s" % (fc, id0))
    try:
      mkdir("part1s/%d_%s" % (fc, id0))
    except:
      pass
    f = open("part1s/%d_%s/movable_part1.sed" % (fc, id0), 'wb')
    f.write(b'\x00' * 4096)
    f.seek(0)
    f.write(lfcs)
    f.seek(16)
    f.write(bytes(id0, 'ascii'))
    f.close()
    print("Written to part1s/%d_%s/movable_part1.sed" % (fc, id0))

serversocket.bind(("", port))
serversocket.listen(5)
print("Waiting for connection from 3ds on port %d ..." % port)
(clientsocket, address) = serversocket.accept()
print("Accepted connection from %s" % address[0])
t = threading.Thread(target=readd, args=(clientsocket,), daemon=True)
t.start()
print("Leave friendcode input empty to terminate program")
while True:
  fc = input("Enter an FC: ").replace("-", "").replace(" ", "")
  if len(fc) == 0:
    choice = input("Terminate the program? ").lower()
    if choice == "yes" or choice == "y":
      break
    continue
  if len(fc) != 12:
    print("That doesn't look like an FC")
    continue
  try:
    fc = int(fc)
  except:
    print("That doesn't look like an FC")
    continue
  print(fc)
  if not verify_fc(fc):
    print("That's an invalid FC")
    continue
  id0 = input("Enter the cooresponding id0: ").replace(" ", "")
  if len(id0) != 32:
    print("That doesn't look like an id0")
    continue
  try:
    int(id0, 16)
  except:
    print("That doesn't look like an id0")
    continue
  fcid0dict[fc] = id0
  clientsocket.send(int.to_bytes(fc, 8, 'little'))
  print("Sent!")
print("Closing connection ...")
clientsocket.close()
serversocket.close()

