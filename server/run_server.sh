#!/usr/bin/env bash

g++ -std=c++20 -c send.cpp -o send.o
g++ -std=c++20 -c recv.cpp -o recv.o
g++ -std=c++20 -c server.cpp -o server.o
g++ send.o recv.o server.o -o server
rm *.o
./server
