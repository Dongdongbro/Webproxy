# Makefile for Proxy Lab
#
# You may modify this file any way you like (except for the handin
# rule). You instructor will type "make" on your specific Makefile to
# build your proxy from sources.

CC = gcc

make: echoserveri

test:
	./echoserveri 5230

csapp.o: csapp.c csapp.h
	$(CC) -c csapp.c

echoserveri.o: echoserveri.c csapp.h
	$(CC) -c echoserveri.c

echoserveri: echoserveri.o csapp.o
	$(CC) echoserveri.o csapp.o -o echoserveri

# Creates a tarball in ../proxylab-handin.tar that you can then
# hand in. DO NOT MODIFY THIS!

clean:
	rm -rf *.o echoserveri

