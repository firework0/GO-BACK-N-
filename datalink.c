#include <stdio.h>
#include <string.h>

#include "protocol.h"
#include "datalink.h"

#define DATA_TIMER 2000

#define WINDOW_SIZE 4
#define MAX_SEQ 128

struct FRAME {
	unsigned char kind; /* FRAME_DATA */
	unsigned char ack;
	unsigned char seq;
	unsigned char data[PKT_LEN];
	unsigned int padding;
};

//原样例程序的变量
static unsigned char frame_nr = 0, buffer[PKT_LEN], nbuffered;
static unsigned char frame_expected = 0;
static int phl_ready = 1;

//滑动窗口的变量
//发送方
static unsigned char window_buzy = 0;
static unsigned char acked_seq =0; //连续收到确认的帧里序号最大的，acked_seq+1为窗口下沿
static unsigned char next_seq_to_send =0; //下一个要发送的帧从未发送过的帧，next_seq_to_send-1为窗口上沿，next_seq_to_send不在窗口内
static struct FRAME send_buffer[MAX_SEQ]; // 缓存已发送但未确认的帧，这个需不需要暂且不知道，简化后貌似不需要缓存
static unsigned char timer_active[MAX_SEQ];//标志计时器状态
static unsigned char pending_packet[PKT_LEN];//缓存网络层ready但窗口已满导致无法发送的帧（这个其实就是buffer，用buffer就行）

//接收方
static unsigned char excepted_seq =0; //期望收到的下个序号，在简化后只要等于收到的序号的下一个就行，因为ack帧的丢失可能会导致这个值往回走

static void put_frame(unsigned char *frame, int len) //发送帧
{
	*(unsigned int *)(frame + len) = crc32(frame, len);
	send_frame(frame, len + 4);
	phl_ready = 0;
}

static void send_data_frame(void)
{
	struct FRAME s;

	s.kind = FRAME_DATA;
	s.seq = next_seq_to_send;
	s.ack = excepted_seq; 
	memcpy(s.data, buffer, PKT_LEN);

	dbg_frame("Send DATA %d %d, ID %d\n", s.seq, s.ack, *(short *)s.data);

	    // 缓存帧副本（以备重传）
	send_buffer[next_seq_to_send] = s;

	put_frame((unsigned char *)&s, 3 + PKT_LEN);

	 // 启动该序号的定时器
	start_timer(next_seq_to_send, DATA_TIMER);
	timer_active[next_seq_to_send] = 1;
}

static void send_ack_frame(unsigned char ack)
{
	struct FRAME s;

	s.kind = FRAME_ACK;
	s.ack = ack; //到时候再决定吧

	dbg_frame("Send ACK  %d\n", s.ack);

	put_frame((unsigned char *)&s, 2);
}

/* 序号区间判断（模 MAX_SEQ） */
static int seq_between(unsigned char a, unsigned char start, unsigned char end)
{
	if (start <= end)
		return start <= a && a < end;
	else
		return a >= start || a < end;
}

//重传函数
static void retransmit_all(void)
{
	unsigned char seq = acked_seq;
	while (seq != next_seq_to_send) {
		if (timer_active[seq]) {
			stop_timer(seq); // 先停止旧的定时器
			timer_active[seq] = 0;
		}
		dbg_event("Retransmit seq=%d\n", seq);
		// 从缓存中取出帧重新发送
		struct FRAME *f = &send_buffer[seq];
		put_frame((unsigned char *)f, 3 + PKT_LEN);
		start_timer(seq, DATA_TIMER);
		timer_active[seq] = 1;
		seq = (seq + 1) % MAX_SEQ;
	}
}

int main(int argc, char **argv)
{
	int event, arg;
	struct FRAME f;
	int len = 0;
	int has_packet = 0;

	protocol_init(argc, argv);
	lprintf("Designed by Jiang Yanjun, build: " __DATE__ "  "__TIME__
		"\n");

	disable_network_layer();

	for (;;) {
		event = wait_for_event(&arg);

		switch (event) {
		case NETWORK_LAYER_READY:   //网络层ready
			get_packet(buffer);  //此处依旧使用buffer读取帧，不修改原来的send_data_frame函数使用buffer的操作
			has_packet = 1;

			if (!window_buzy&&phl_ready)
			{
				send_data_frame();
				next_seq_to_send = (next_seq_to_send + 1) % MAX_SEQ;
				has_packet = 0;
				if ((next_seq_to_send - acked_seq + MAX_SEQ) % MAX_SEQ >=WINDOW_SIZE) {
					window_buzy = 1;
				}
			}
			break;

		case PHYSICAL_LAYER_READY:   //物理层ready
			phl_ready = 1;
			if (has_packet && !window_buzy) {
				send_data_frame();
				next_seq_to_send = (next_seq_to_send + 1) % MAX_SEQ;
				has_packet = 0;
				if ((next_seq_to_send - acked_seq + MAX_SEQ) % MAX_SEQ >=WINDOW_SIZE) {
					window_buzy = 1;
				}
			}
			break;

		case FRAME_RECEIVED: {
			int len = recv_frame((unsigned char *)&f, sizeof f);
			if (len < 5 || crc32((unsigned char *)&f, len) != 0) {
				dbg_event("CRC error\n");
				break;
			}

			//处理确认信息（无论数据帧还是ACK帧）
			unsigned char ack_num = f.ack;
			// 累积确认：ack_num 表示对方期望的下一个序号，即已确认 ack_num-1 及之前的所有帧
			// 只有 ack_num 在 (acked_seq, next_seq] 范围内才有效
			if (seq_between(ack_num, acked_seq + 1, next_seq_to_send + 1)) {
				// 停止从 acked_seq 到 ack_num-1 的所有定时器
				for (unsigned char s = acked_seq; s != ack_num;
				     s = (s + 1) % MAX_SEQ) {
					if (timer_active[s]) {
						stop_timer(s);
						timer_active[s] = 0;
					}
				}
				// 滑动窗口下沿到 ack_num
				acked_seq = ack_num;
				// 窗口可能不再满，允许网络层继续发送
				window_buzy = 0;
			}

			//数据帧，处理接收
			if (f.kind == FRAME_DATA) {
				dbg_frame("Recv DATA seq=%d ack=%d pktID=%d\n",f.seq, f.ack, *(short *)f.data);
				if (f.seq == excepted_seq) {
					// 正确接收
					put_packet(f.data,PKT_LEN); //固定长度用 PKT_LEN 
					excepted_seq =(excepted_seq + 1) % MAX_SEQ;
					// 发送 ACK，确认收到的帧（发送期望序号）
					send_ack_frame(excepted_seq);
				} else {
					// 乱序或重复帧，Go-Back-N 丢弃，可发送一个重复 ACK 提醒对方
				}
			}
			//ACK 帧无需额外处理
			else if (f.kind == FRAME_ACK) {
				dbg_frame("Recv ACK ack=%d\n", f.ack);
				// 确认信息已在第一步处理，这里只打印
			}
			break;
		}

		case DATA_TIMEOUT:   //超时
			dbg_event("---- DATA %d timeout\n", arg);
			retransmit_all();
			break;
		}

		if (!window_buzy && phl_ready)
			enable_network_layer();
		else
			disable_network_layer();
	}
}
