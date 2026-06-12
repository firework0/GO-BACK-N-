/* datalink.h - 帧类型定义 */
#ifndef DATALINK_H
#define DATALINK_H

/* 帧类型 */
#define FRAME_DATA 1
#define FRAME_ACK 2
#define FRAME_NAK 3

/* 
 * 帧格式说明：
 * DATA Frame:
 *   +--------+-------+-------+--------+----------+--------+
 *   | kind(1)| seq(1)| ack(1)| len(1) | data(256)| CRC(4) |
 *   +--------+-------+-------+--------+----------+--------+
 * ACK/NAK Frame:
 *   +--------+-------+-------+--------+----------+--------+
 *   | kind(1)| ack(1)| (seq=0)| len=0 | (no data)| CRC(4) |
 *   +--------+-------+-------+--------+----------+--------+
 */

#endif

