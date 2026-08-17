#include "server.hpp"

//TODO: main app (server) logic; 2 threads: 1 for sending and the other for reading

int main() {
    cap_init().display();
    init_network("127.0.0.1", 5000).display();

    // snd() blocks until the desktop draws, so this paces itself
    while (true) {
        log_entry le = snd();
        if (!le.status) { le.display(); break; }
    }

    // rcv().display(); //dbg

    cleanup_network().display();
    cap_end().display();
    return 0;
}
