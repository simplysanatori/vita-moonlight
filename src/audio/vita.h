

// Packet duration to ask the host for at the next stream: 10 for 10 ms, anything
// else for 5 ms (common-c's default). Call before LiStartConnection.
void vitaaudio_set_packet_ms(int ms);
// Packet duration the last stream negotiated, in ms (0 before the first one)
int vitaaudio_packet_ms();
void vitaaudio_start();
void vitaaudio_stop();
unsigned int vitaaudio_dropped_packets();
