# Makefile - gnfsd (simple NFSv2 server). Linux host tool.
#   make            # build ./gnfsd
#   make test       # build + run the direct-RPC test (no root/mount)
#   make stress     # LINK churn + 32MB write-integrity stress

CC     = gcc
CFLAGS = -Wall -O2 -D_GNU_SOURCE
OBJ    = xdr.o rpc.o handle.o nfs.o main.o

gnfsd: $(OBJ)
	$(CC) $(CFLAGS) -o gnfsd $(OBJ)

$(OBJ): xdr.h rpc.h handle.h nfs.h

.c.o:
	$(CC) $(CFLAGS) -c $< -o $@

test: gnfsd
	./test-nfs.sh
	./test-persist.sh

stress: gnfsd
	./test-stress.sh

clean:
	rm -f $(OBJ) gnfsd
