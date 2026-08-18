g++ -std=c++20 -c send.cpp -o send.o
g++ -std=c++20 -c recv.cpp -o recv.o
g++ -std=c++20 -c server.cpp -o server.o
g++ send.o recv.o server.o -o server -ld3d11 -ldxgi -ld3d10 -lws2_32 "vpl.lib"
del *.o
.\server.exe
