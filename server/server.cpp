#include "server.hpp"

//TODO: main app (server) logic; 2 threads: 1 for sending and the other for reading

int main() {
    cap_init().display();
    init_network("127.0.0.1", 5000).display();
    //TODO: send the dimensions raw

    log_entry le = snd();
    le.display();

    cleanup_network().display();
    cap_end().display();
    return 0;
}
