#define main miku_program_main
#include "baseline-source.c"
#undef main
int main(void) {
    int pair[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair)) return 1;
    g_audio_ipc = pair[0];
    double start = now_seconds();
    double pos = audio_time_pos();
    printf("baseline missing reply: %.1f ms, position %.1f\n",
           (now_seconds() - start) * 1000, pos);
    const char *reply = "{\"data\":null}\n";
    if (write(pair[1], reply, strlen(reply)) < 0) return 1;
    printf("baseline null position: %.1f\n", audio_time_pos());
    return 0;
}
