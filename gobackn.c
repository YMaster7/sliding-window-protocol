#include <stdbool.h>
#include "common.h"
#include "protocol.h"

static bool phl_ready = false;

/**
 * @brief 返回 a \<= b \< c (循环)
 * @param a
 * @param b
 * @param c
 * @return true / false
 */
static bool between(seq_t a, seq_t b, seq_t c) {
    return ((a <= b) && (b < c)) || ((c < a) && (a <= b)) || ((b < c) && (c < a));
}

/**
 * @brief 使用 crc32 校验数据完整性
 * @param data 数据指针
 * @param len 数据长度
 * @return 数据完整返回 true, 否则返回 false
 */
static bool crc32_check(void *data, int len) {
    if (crc32((uchar_t *) data, len) != 0) {
        return false;
    }
    return true;
}

/**
 * @brief 生成 CRC 校验并发送到物理层
 * @param frame 没有 CRC 校验的帧
 * @param len 帧长度
 */
static void put_frame(void *frame, int len) {
    *(uint32_t *) (frame + len) = crc32(frame, len);

    send_frame(frame, len + 4);  // sizeof(uint32_t) = 4
    phl_ready = false;
}

/**
 * @brief 发送数据帧
 * @param frame_nr 帧在 buffer 中的序号
 * @param frame_expected 期望接受的帧序号, 用于生成 piggyback ack
 * @param buffer 帧缓冲区
 */
static void send_data_frame(seq_t frame_nr, seq_t frame_expected, packet_t buffer[]) {
    struct frame f = {
            .kind = FRAME_DATA,
            .ack = (frame_expected + MAX_SEQ) % (MAX_SEQ + 1),  // piggyback ack
            .seq = frame_nr,
    };
    memcpy(f.data, buffer[frame_nr], sizeof(f.data));

    dbg_frame("Sending DATA frame <ack=%d, seq=%d, id=%d>\n", f.ack, f.seq, *(short *) f.data);

    put_frame(&f, 259);  // 259 = 1 + 1 + 1 + 256
    start_timer(frame_nr, DATA_TIMEOUT_MS);
    stop_ack_timer();
}

/**
 * @brief 发送 ack 帧
 * @param frame_expected 期望接受的帧序号, 用于生成 ack 序号
 */
static void send_ack_frame(seq_t frame_expected) {
    struct frame f = {
            .kind = FRAME_ACK,
            .ack = (frame_expected + MAX_SEQ) % (MAX_SEQ + 1),
    };

    dbg_frame("Sending ACK frame <ack=%d>\n", f.ack);

    put_frame(&f, 2);  // 2 = 1 + 1
}

/**
 * @brief 发送 nak 帧
 * @param frame_expected 期望接受的帧序号, 用于生成 ack 序号
 */
static void send_nak_frame(seq_t frame_expected) {
    struct frame f = {
            .kind = FRAME_NAK,
            .ack = (frame_expected + MAX_SEQ) % (MAX_SEQ + 1),
    };

    dbg_frame("Sending NAK frame <ack=%d>\n", f.ack);

    put_frame(&f, 2);  // 2 = 1 + 1
}

int main(int argc, char **argv) {
    int arg;  // *_TIMEOUT 事件中产生超时事件的定时器编号
    int event;
    int len;  // 接收到的数据帧长度

    // 发送窗口
    seq_t next_frame_to_send = 0;
    seq_t ack_expected = 0;

    // 接收窗口
    seq_t frame_expected = 0;
    bool no_nak = true;  // 是否发送过 nak 帧

    struct frame f;  // 接收到的帧
    packet_t buffer[MAX_SEQ + 1];  // 出境 buffer
    seq_t buffer_len = 0;  // 当前 buffer 中的 packet 数

    protocol_init(argc, argv);
    lprintf("Go back N protocol by Yin Rui, build %s %s\n", __DATE__, __TIME__);

    disable_network_layer();

    while (true) {
        event = wait_for_event(&arg);

#ifndef NDEBUG
        if (event != PHYSICAL_LAYER_READY) {
            lprintf("ack = %d, next = %d, inbound = %d\n", ack_expected, next_frame_to_send,
              frame_expected);
            lprintf("buffer = %d\n", buffer_len);
        }
#endif

        switch (event) {
            case NETWORK_LAYER_READY:
                get_packet(buffer[next_frame_to_send]);
                buffer_len++;
                send_data_frame(next_frame_to_send, frame_expected, buffer);
                inc(next_frame_to_send);
                break;

            case PHYSICAL_LAYER_READY:
                phl_ready = true;
                break;

            case FRAME_RECEIVED:
                len = recv_frame((uchar_t *) &f, sizeof(f));
                if (f.kind == FRAME_ACK) {
                    dbg_frame("Received ACK frame <ack=%d>\n", f.ack);
                } else if (f.kind == FRAME_NAK) {
                    dbg_frame("Received NAK frame <ack=%d>\n", f.ack);
                } else if (f.kind == FRAME_DATA) {
                    dbg_frame("Received DATA frame <ack=%d, seq=%d, id=%d>\n", f.ack, f.seq,
                              *(short *) f.data);
                }

                if (!crc32_check(&f, len)) {
                    dbg_event("*** Bad CRC Checksum ***\n");

                    if (f.kind == FRAME_DATA && no_nak) {
                        send_nak_frame(frame_expected);
                        no_nak = false;
                    }

                    break;
                }

                if (f.kind == FRAME_DATA) {
                    if (f.seq == frame_expected) {
                        put_packet(f.data, sizeof(f.data));
                        start_ack_timer(ACK_TIMEOUT_MS);
                        inc(frame_expected);
                        no_nak = true;
                    } else if (no_nak) {
                        send_nak_frame(frame_expected);
                        no_nak = false;
                    }
                }

                if (f.kind != FRAME_NAK) {
                    // 累积确认
                    while (between(ack_expected, f.ack, next_frame_to_send)) {
                        stop_timer(ack_expected);
                        buffer_len--;
                        inc(ack_expected);
                    }
                } else {
                    next_frame_to_send = ack_expected;
                    for (seq_t i = 0; i < buffer_len; i++) {
                        send_data_frame(next_frame_to_send, frame_expected, buffer);
                        inc(next_frame_to_send);
                    }
                }

                break;

            case DATA_TIMEOUT:
                dbg_event("DATA frame <seq=%d> timeout\n", arg);

                next_frame_to_send = ack_expected;
                for (seq_t i = 0; i < buffer_len; i++) {
                    send_data_frame(next_frame_to_send, frame_expected, buffer);
                    inc(next_frame_to_send);
                }
                break;

            case ACK_TIMEOUT:
                send_ack_frame(frame_expected);
                stop_ack_timer();
                break;

            default:
                dbg_warning("Unknown event %d\nAbort.\n", event);
                return 1;
        }

        if (buffer_len < MAX_SEQ && phl_ready) {
            enable_network_layer();
        } else {
            disable_network_layer();
        }
    }
}