# Makefile - gnfsd (simple NFSv2 server). Linux host tool.
#   make            # build ./gnfsd
#   make test       # build + run the handle-table unit test and the
#                   # direct-RPC tests (no root/mount)
#   make stress     # LINK churn + 32MB write-integrity stress, and the
#                   # restart-walk linearity check (20k vs 80k entries)

CC     = gcc
CFLAGS = -Wall -O2 -D_GNU_SOURCE
OBJ    = xdr.o rpc.o handle.o nfs.o main.o

gnfsd: $(OBJ)
	$(CC) $(CFLAGS) -o gnfsd $(OBJ)

$(OBJ): xdr.h rpc.h handle.h nfs.h

.c.o:
	$(CC) $(CFLAGS) -c $< -o $@

test-handle: test-handle.c handle.c handle.h
	$(CC) $(CFLAGS) -o test-handle test-handle.c

test: gnfsd test-handle
	./test-handle
	./test-nfs.sh
	./test-persist.sh

stress: gnfsd
	./test-stress.sh
	./test-rebuild.sh

clean:
	rm -f $(OBJ) gnfsd test-handle
