/*
 * Intel Wireless WiMAX Connection 2400m
 * Generic (non-bus specific) TX handling
 *
 *
 * Copyright (C) 2007-2008 Intel Corporation. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 *   * Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in
 *     the documentation and/or other materials provided with the
 *     distribution.
 *   * Neither the name of Intel Corporation nor the names of its
 *     contributors may be used to endorse or promote products derived
 *     from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 *
 * Intel Corporation <linux-wimax@intel.com>
 * Yanir Lubetkin <yanirx.lubetkin@intel.com>
 *  - Initial implementation
 *
 * Intel Corporation <linux-wimax@intel.com>
 * Inaky Perez-Gonzalez <inaky.perez-gonzalez@intel.com>
 *  - Rewritten to use a single FIFO to lower the memory allocation
 *    pressure and optimize cache hits when copying to the queue, as
 *    well as splitting out bus-specific code.
 *
 *
 * Implements data transmission to the device; this is done through a
 * software FIFO, as data/control frames can be coalesced (while the
 * device is reading the previous tx transaction, others accumulate).
 *
 * A FIFO is used because at the end it is resource-cheaper that trying
 * to implement scatter/gather over USB. As well, most traffic is going
 * to be download (vs upload).
 *
 * The format for sending/receiving data to/from the i2400m is
 * described in detail in rx.c:PROTOCOL FORMAT. In here we implement
 * the transmission of that. This is split between a bus-independent
 * part that just prepares everything and a bus-specific part that
 * does the actual transmission over the bus to the device (in the
 * bus-specific driver).
 *
 *
 * The general format of a device-host transaction is MSG-HDR, PLD1,
 * PLD2...PLDN, PL1, PL2,...PLN, PADDING.
 *
 * Because we need the send payload descriptors and then payloads and
 * because it is kind of expensive to do scatterlists in USB (one URB
 * per node), it becomes cheaper to append all the data to a FIFO
 * (copying to a FIFO potentially in cache is cheaper).
 *
 * Then the bus-specific code takes the parts of that FIFO that are
 * written and passes them to the device.
 *
 * So the concepts to keep in mind there are:
 *
 * We use a FIFO to queue the data in a linear buffer. We first append
 * a MSG-HDR, space for I2400M_TX_PLD_MAX payload descriptors and then
 * go appending payloads until we run out of space or of payload
 * descriptors. Then we append padding to make the whole transaction a
 * multiple of i2400m->bus_tx_block_size (as defined by the bus layer).
 *
 * - A TX message: a combination of a message header, payload
 *   descriptors and payloads.
 *
 *     Open: it is marked as active (i2400m->tx_msg is valid) and we
 *       can keep adding payloads to it.
 *
 *     Closed: we are not appending more payloads to this TX message
 *       (exahusted space in the queue, too many payloads or
 *       whichever).  We have appended padding so the whole message
 *       length is aligned to i2400m->bus_tx_block_size (as set by the
 *       bus/transport layer).
 *
 * - Most of the time we keep a TX message open to which we append
 *   payloads.
 *
 * - If we are going to append and there is no more space (we are at
 *   the end of the FIFO), we close the message, mark the rest of the
 *   FIFO space unusable (skip_tail), create a new message at the
 *   beginning of the FIFO (if there is space) and append the message
 *   there.
 *
 *   This is because we need to give linear TX messages to the bus
 *   engine. So we don't write a message to the remaining FIFO space
 *   until the tail and continue at the head of it.
 *
 * - We overload one of the fields in the message header to use it as
 *   'size' of the TX message, so we can iterate over them. It also
 *   contains a flag that indicates if we have to skip it or not.
 *   When we send the buffer, we update that to its real on-the-wire
 *   value.
 *
 * - The MSG-HDR PLD1...PLD2 stuff has to be a size multiple of 16.
 *
 *   It follows that if MSG-HDR says we have N messages, the whole
 *   header + descriptors is 16 + 4*N; for those to be a multiple of
 *   16, it follows that N can be 4, 8, 12, ... (32, 48, 64, 80...
 *   bytes).
 *
 *   So if we have only 1 payload, we have to submit a header that in
 *   all truth has space for 4.
 *
 *   The implication is that we reserve space for 12 (64 bytes); but
 *   if we fill up only (eg) 2, our header becomes 32 bytes only. So
 *   the TX engine has to shift those 32 bytes of msg header and 2
 *   payloads and padding so that right after it the payloads start
 *   and the TX engine has to know about that.
 *
 *   It is cheaper to move the header up than the whole payloads down.
 *
 *   We do this in i2400m_tx_close(). See 'i2400m_msg_hdr->offset'.
 *
 * - Each payload has to be size-padded to 16 bytes; before appending
 *   it, we just do it.
 *
 * - The whole message has to be padded to i2400m->bus_tx_block_size;
 *   we do this at close time. Thus, when reserving space for the
 *   payload, we always make sure there is also free space for this
 *   padding that sooner or later will happen.
 *
 * When we append a message, we tell the bus specific code to kick in
 * TXs. It will TX (in parallel) until the buffer is exhausted--hence
 * the lockin we do. The TX code will only send a TX message at the
 * time (which remember, might contain more than one payload). Of
 * course, when the bus-specific driver attempts to TX a message that
 * is still open, it gets closed first.
 *
 * Gee, this is messy; well a picture. In the example below we have a
 * partially full FIFO, with a closed message ready to be delivered
 * (with a moved message header to make sure it is size-aligned to
 * 16), TAIL room that was unusable (and thus is marked with a message
 * header that says 'skip this') and at the head of the buffer, an
 * imcomplete message with a couple of payloads.
 *
 * N   ___________________________________________________
 *    |                                                   |
 *    |     TAIL room                                     |
 *    |                                                   |
 *    |  msg_hdr to skip (size |= 0x80000)                |
 *    |---------------------------------------------------|-------
 *    |                                                   |  /|\
 *    |                                                   |   |
 *    |  TX message padding                               |   |
 *    |                                                   |   |
 *    |                                                   |   |
 *    |- - - - - - - - - - - - - - - - - - - - - - - - - -|   |
 *    |                                                   |   |
 *    |  payload 1                                        |   |
 *    |                                                   | N * tx_block_size
 *    |                                                   |   |
 *    |- - - - - - - - - - - - - - - - - - - - - - - - - -|   |
 *    |                                                   |   |
 *    |  payload 1                                        |   |
 *    |                                                   |   |
 *    |                                                   |   |
 *    |- - - - - - - - - - - - - - - - - - - - - - - - - -|- -|- - - -
 *    |  padding 3                  /|\                   |   |   /|\
 *    |  padding 2                   |                    |   |    |
 *    |  pld 1                32 bytes (2 * 16)           |   |    |
 *    |  pld 0                       |                    |   |    |
 *    |  moved msg_hdr              \|/                   |  \|/   |
 *    |- - - - - - - - - - - - - - - - - - - - - - - - - -|- - -   |
 *    |                                                   |    _PLD_SIZE
 *    |  unused                                           |        |
 *    |                                                   |        |
 *    |- - - - - - - - - - - - - - - - - - - - - - - - - -|        |
 *    |  msg_hdr (size X)       [this message is closed]  |       \|/
 *    |===================================================|========== <=== OUT
 *    |                                                   |
 *    |                                                   |
 *    |                                                   |
 *    |          Free rooom                               |
 *    |                                                   |
 *    |                                                   |
 *    |                                                   |
 *    |                                                   |
 *    |                                                   |
 *    |                                                   |
 *    |                                                   |
 *    |                                                   |
 *    |                                                   |
 *    |===================================================|========== <=== IN
 *    |                                                   |
 *    |                                                   |
 *    |                                                   |
 *    |                                                   |
 *    |  payload 1                                        |
 *    |                                                   |
 *    |                                                   |
 *    |- - - - - - - - - - - - - - - - - - - - - - - - - -|
 *    |                                                   |
 *    |  payload 0                                        |
 *    |                                                   |
 *    |                                                   |
 *    |- - - - - - - - - - - - - - - - - - - - - - - - - -|
 *    |  pld 11                     /|\                   |
 *    |  ...                         |                    |
 *    |  pld 1                64 bytes (2 * 16)           |
 *    |  pld 0                       |                    |
 *    |  msg_hdr (size X)           \|/ [message is open] |
 * 0   ---------------------------------------------------
 *
 *
 * ROADMAP
 *
 * i2400m_tx_setup()           Called by i2400m_setup
 * i2400m_tx_release()         Called by i2400m_release()
 *
 *  i2400m_tx()                 Called to send data or control frames
 *    i2400m_tx_fifo_push()     Allocates append-space in the FIFO
 *    i2400m_tx_new()           Opens a new message in the FIFO
 *    i2400m_tx_fits()          Checks if a new payload fits in the message
 *    i2400m_tx_close()         Closes an open message in the FIFO
 *    i2400m_tx_skip_tail()     Marks unusable FIFO tail space
 *    i2400m->bus_tx_kick()
 *
 * Now i2400m->bus_tx_kick() is the the bus-specific driver backend
 * implementation; that would do:
 *
 * i2400m->bus_tx_kick()
 *   i2400m_tx_msg_get()	Gets first message ready to go
 *   ...sends it...
 *   i2400m_tx_msg_sent()       Ack the message is sent; repeat from
 *                              _tx_msg_get() until it returns NULL
 *                               (FIFO empty).
 */
#include <linux/netdevice.h>
#include "i2400m.h"


#define D_SUBMODULE tx
#include "debug-levels.h"

enum {
	/**
	 * TX Buffer size
	 *
	 * Doc says maximum transaction is 16KiB. If we had 16KiB en
	 * route and 16KiB being queued, it boils down to needing
	 * 32KiB.
	 */
	I2400M_TX_BUF_SIZE = 32768,
	/**
	 * Message header and payload descriptors have to be 16
	 * aligned (16 + 4 * N = 16 * M). If we take that average sent
	 * packets are MTU size (~1400-~1500) it follows that we could
	 * fit at most 10-11 payloads in one transaction. To meet the
	 * alignment requirement, that means we need to leave space
	 * for 12 (64 bytes). To simplify, we leave space for that. If
	 * at the end there are less, we pad up to the nearest
	 * multiple of 16.
	 */
	I2400M_TX_PLD_MAX = 12,
	I2400M_TX_PLD_SIZE = sizeof(struct i2400m_msg_hdr)
	+ I2400M_TX_PLD_MAX * sizeof(struct i2400m_pld),
	I2400M_TX_SKIP = 0x80000000,
};

#define TAIL_FULL ((void *)~(unsigned long)NULL)

/*
 * Allocate @size bytes in the TX fifo, return a pointer to it
 *
 * @i2400m: device descriptor
 * @size: size of the buffer we need to allocate
 * @padding: ensure that there is at least this many bytes of free
 *     contiguous space in the fifo. This is needed because later on
 *     we might need to add padding.
 *
 * Returns:
 *
 *     Pointer to the allocated space. NULL if there is no
 *     space. TAIL_FULL if there is no space at the tail but there is at
 *     the head (Case B below).
 *
 * These are the two basic cases we need to keep an eye for -- it is
 * much better explained in linux/kernel/kfifo.c, but this code
 * basically does the same. No rocket science here.
 *
 *       Case A               Case B
 * N  ___________          ___________
 *   | tail room |        |   data    |
 *   |           |        |           |
 *   |<-  IN   ->|        |<-  OUT  ->|
 *   |           |        |           |
 *   |   data    |        |   room    |
 *   |           |        |           |
 *   |<-  OUT  ->|        |<-  IN   ->|
 *   |           |        |           |
 *   | head room |        |   data    |
 * 0  -----------          -----------
 *
 * We allocate only *contiguous* space.
 *
 * We can allocate only from 'room'. In Case B, it is simple; in case
 * A, we only try from the tail room; if it is not enough, we just
 * fail and return TAIL_FULL and let the caller figure out if we wants to
 * skip the tail room and try to allocate from the head.
 *
 * Note:
 *
 *     Assumes i2400m->tx_lock is taken, and we use that as a barrier
 *
 *     The indexes keep increasing and we reset them to zero when we
 *     pop data off the queue
 */
static
void *i2400m_tx_fifo_push(struct i2400m *i2400m, size_t size, size_t padding)
{
	struct device *dev = i2400m_dev(i2400m);
	size_t room, tail_room, needed_size;
	void *ptr;

	needed_size = size + padding;
	room = I2400M_TX_BUF_SIZE - (i2400m->tx_in - i2400m->tx_out);
	if (room < needed_size)	{ /* this takes care of Case B */
		d_printf(2, dev, "fifo push %zu/%zu: no space\n",
			 size, padding);
		return NULL;
	}
	/* Is there space at the tail? */
	tail_room = I2400M_TX_BUF_SIZE - i2400m->tx_in % I2400M_TX_BUF_SIZE;
	if (tail_room < needed_size) {
		if (i2400m->tx_out % I2400M_TX_BUF_SIZE
		    < i2400m->tx_in % I2400M_TX_BUF_SIZE) {
			d_printf(2, dev, "fifo push %zu/%zu: tail full\n",
				 size, padding);
			return TAIL_FULL;	/* There might be head space */
		} else {
			d_printf(2, dev, "fifo push %zu/%zu: no head space\n",
				 size, padding);
			return NULL;	/* There is no space */
		}
	}
	ptr = i2400m->tx_buf + i2400m->tx_in % I2400M_TX_BUF_SIZE;
	d_printf(2, dev, "fifo push %zu/%zu: at @%zu\n", size, padding,
		 i2400m->tx_in % I2400M_TX_BUF_SIZE);
	i2400m->tx_in += size;
	return ptr;
}


/*
 * Mark the tail of the FIFO buffer as 'to-skip'
 *
 * We should never hit the BUG_ON() because all the sizes we push to
 * the FIFO are padded to be a multiple of 16 -- the size of *msg
 * (I2400M_PL_PAD for the payloads, I2400M_TX_PLD_SIZE for the
 * header).
 *
 * Note:
 *
 *     Assumes i2400m->tx_lock is taken, and we use that as a barrier
 */
static
void i2400m_tx_skip_tail(struct i2400m *i2400m)
{
	struct device *dev = i2400m_dev(i2400m);
	size_t tx_in = i2400m->tx_in % I2400M_TX_BUF_SIZE;
	size_t tail_room = I2400M_TX_BUF_SIZE - tx_in;
	struct i2400m_msg_hdr *msg = i2400m->tx_buf + tx_in;
	BUG_ON(tail_room < sizeof(*msg));
	msg->size = tail_room | I2400M_TX_SKIP;
	d_printf(2, dev, "skip tail: skipping %zu bytes @%zu\n",
		 tail_room, tx_in);
	i2400m->tx_in += tail_room;
}


/*
 * Check if a skb will fit in the TX queue's current active TX
 * message (if there are still descriptors left unused).
 *
 * Returns:
 *     0 if the message won't fit, 1 if it will.
 *
 * Note:
 *
 *     Assumes a TX message is active (i2400m->tx_msg).
 *
 *     Assumes i2400m->tx_lock is taken, and we use that as a barrier
 */
static
unsigned i2400m_tx_fits(struct i2400m *i2400m)
{
	struct i2400m_msg_hdr *msg_hdr = i2400m->tx_msg;
	return le16_to_cpu(msg_hdr->num_pls) < I2400M_TX_PLD_MAX;

}


/*
 * Start a new TX message header in the queue.
 *
 * Reserve memory from the base FIFO engine and then just initialize
 * the message header.
 *
 * We allocate the biggest TX message header we might need (one that'd
 * fit I2400M_TX_PLD_MAX payloads) -- when it is closed it will be
 * 'ironed it out' and the unneeded parts removed.
 *
 * NOTE:
 *
 *     Assumes that the previous message is CLOSED (eg: either
 *     there was none or 'i2400m_tx_close()' was called on it).
 *
 *     Assumes i2400m->tx_lock is taken, and we use that as a barrier
 */
static
void i2400m_tx_new(struct i2400m *i2400m)
{
	struct device *dev = i2400m_dev(i2400m);
	struct i2400m_msg_hdr *tx_msg;
	BUG_ON(i2400m->tx_msg != NULL);
try_head:
	tx_msg = i2400m_tx_fifo_push(i2400m, I2400M_TX_PLD_SIZE, 0);
	if (tx_msg == NULL)
		goto out;
	else if (tx_msg == TAIL_FULL) {
		i2400m_tx_skip_tail(i2400m);
		d_printf(2, dev, "new TX message: tail full, trying head\n");
		goto try_head;
	}
	memset(tx_msg, 0, I2400M_TX_PLD_SIZE);
	tx_msg->size = I2400M_TX_PLD_SIZE;
out:
	i2400m->tx_msg = tx_msg;
	d_printf(2, dev, "new TX message: %p @%zu\n",
		 tx_msg, (void *) tx_msg - i2400m->tx_buf);
}


/*
 * Finalize the current TX message header
 *
 * Sets the message header to be at the proper location depending on
 * how many descriptors we have (check documentation at the file's
 * header for more info on that).
 *
 * Appends padding bytes to make sure the whole TX message (counting
 * from the 'relocated' message header) is aligned to
 * tx_block_size. We assume the _append() code has left enough space
 * in the FIFO for that. If there are no payloads, just pass, as it
 * won't be transferred.
 *
 * The amount of padding bytes depends on how many payloads are in the
 * TX message, as the "msg header and payload descriptors" will be
 * shifted up in the buffer.
 */
static
void i2400m_tx_close(struct i2400m *i2400m)
{
	struct device *dev = i2400m_dev(i2400m);
	struct i2400m_msg_hdr *tx_msg = i2400m->tx_msg;
	struct i2400m_msg_hdr *tx_msg_moved;
	size_t aligned_size, padding, hdr_size;
	void *pad_buf;

	if (tx_msg->size & I2400M_TX_SKIP)	/* a skipper? nothing to do */
		goto out;

	/* Relocate the message header
	 *
	 * Find the current header size, align it to 16 and if we need
	 * to move it so the tail is next to the payloads, move it and
	 * set the offset.
	 *
	 * If it moved, this header is good only for transmission; the
	 * original one (it is kept if we moved) is still used to
	 * figure out where the next TX message starts (and where the
	 * offset to the moved header is).
	 */
	hdr_size = sizeof(*tx_msg)
		+ le16_to_cpu(tx_msg->num_pls) * sizeof(tx_msg->pld[0]);
	hdr_size = ALIGN(hdr_size, I2400M_PL_PAD);
	tx_msg->offset = I2400M_TX_PLD_SIZE - hdr_size;
	tx_msg_moved = (void *) tx_msg + tx_msg->offset;
	memmove(tx_msg_moved, tx_msg, hdr_size);
	tx_msg_moved->size -= tx_msg->offset;
	/*
	 * Now figure out how much we have to add to the (moved!)
	 * message so the size is a multiple of i2400m->bus_tx_block_size.
	 */
	aligned_size = ALIGN(tx_msg_moved->size, i2400m->bus_tx_block_size);
	padding = aligned_size - tx_msg_moved->size;
	if (padding > 0) {
		pad_buf = i2400m_tx_fifo_push(i2400m, padding, 0);
		if (unlikely(WARN_ON(pad_buf == NULL
				     || pad_buf == TAIL_FULL))) {
			/* This should not happen -- append should verify
			 * there is always space left at least to append
			 * tx_block_size */
			dev_err(dev,
				"SW BUG! Possible data leakage from memory the "
				"device should not read for padding - "
				"size %lu aligned_size %zu tx_buf %p in "
				"%zu out %zu\n",
				(unsigned long) tx_msg_moved->size,
				aligned_size, i2400m->tx_buf, i2400m->tx_in,
				i2400m->tx_out);
		} else
			memset(pad_buf, 0xad, padding);
	}
	tx_msg_moved->padding = cpu_to_le16(padding);
	tx_msg_moved->size += padding;
	if (tx_msg != tx_msg_moved)
		tx_msg->size += padding;
out:
	i2400m->tx_msg = NULL;
}


/**
 * i2400m_tx - send the data in a buffer to the device
 *
 * @buf: pointer to the buffer to transmit
 *
 * @buf_len: buffer size
 *
 * @pl_type: type of the payload we are sending.
 *
 * Returns:
 *     0 if ok, < 0 errno code on error (-ENOSPC, if there is no more
 *     room for the message in the queue).
 *
 * Appends the buffer to the TX FIFO and notifies the bus-specific
 * part of the driver that there is new data ready to transmit.
 * Once this function returns, the buffer has been copied, so it can
 * be reused.
 *
 * The steps followed to append are explained in detail in the file
 * header.
 *
 * Whenever we write to a message, we increase msg->size, so it
 * reflects exactly how big the message is. This is needed so that if
 * we concatenate two messages before they can be sent, the code that
 * sends the messages can find the boundaries (and it will replace the
 * size with the real barker before sending).
 *
 * Note:
 *
 *     Cold and warm reset payloads need to be sent as a single
 *     payload, so we handle that.
 */
int i2400m_tx(struct i2400m *i2400m, const void *buf, size_t buf_len,
	      enum i2400m_pt pl_type)
{
	int result = -ENOSPC;
	struct device *dev = i2400m_dev(i2400m);
	unsigned long flags;
	size_t padded_len;
	void *ptr;
	unsigned is_singleton = pl_type == I2400M_PT_RESET_WARM
		|| pl_type == I2400M_PT_RESET_COLD;

	d_fnstart(3, dev, "(i2400m %p skb %p [%zu bytes] pt %u)\n",
		  i2400m, buf, buf_len, pl_type);
	padded_len = ALIGN(buf_len, I2400M_PL_PAD);
	d_printf(5, dev, "padded_len %zd buf_len %zd\n", padded_len, buf_len);
	/* If there is no current TX message, create one; if the
	 * current one is out of payload slots or we have a singleton,
	 * close it and start a new one */
	spin_lock_irqsave(&i2400m->tx_lock, flags);
try_new:
	if (unlikely(i2400m->tx_msg == NULL))
		i2400m_tx_new(i2400m);
	else if (unlikely(!i2400m_tx_fits(i2400m)
			  || (is_singleton && i2400m->tx_msg->num_pls != 0))) {
		d_printf(2, dev, "closing TX message (fits %u singleton "
			 "%u num_pls %u)\n", i2400m_tx_fits(i2400m),
			 is_singleton, i2400m->tx_msg->num_pls);
		i2400m_tx_close(i2400m);
		i2400m_tx_new(i2400m);
	}
	if (i2400m->tx_msg->size + padded_len > I2400M_TX_BUF_SIZE / 2) {
		d_printf(2, dev, "TX: message too big, going new\n");
		i2400m_tx_close(i2400m);
		i2400m_tx_new(i2400m);
	}
	if (i2400m->tx_msg == NULL)
		goto error_tx_new;
	/* So we have a current message header; now append space for
	 * the message -- if there is not enough, try the head */
	ptr = i2400m_tx_fifo_push(i2400m, padded_len,
				  i2400m->bus_tx_block_size);
	if (ptr == TAIL_FULL) {	/* Tail is full, try head */
		d_printf(2, dev, "pl append: tail full\n");
		i2400m_tx_close(i2400m);
		i2400m_tx_skip_tail(i2400m);
		goto try_new;
	} else if (ptr == NULL) {	/* All full */
		result = -ENOSPC;
		d_printf(2, dev, "pl append: all full\n");
	} else {			/* Got space, copy it, set padding */
		struct i2400m_msg_hdr *tx_msg = i2400m->tx_msg;
		unsigned num_pls = le16_to_cpu(tx_msg->num_pls);
		memcpy(ptr, buf, buf_len);
		memset(ptr + buf_len, 0xad, padded_len - buf_len);
		i2400m_pld_set(&tx_msg->pld[num_pls], buf_len, pl_type);
		d_printf(3, dev, "pld 0x%08x (type 0x%1x len 0x%04zx\n",
			 le32_to_cpu(tx_msg->pld[num_pls].val),
			 pl_type, buf_len);
		tx_msg->num_pls = le16_to_cpu(num_pls+1);
		tx_msg->size += padded_len;
		d_printf(2, dev, "TX: appended %zu b (up to %u b) pl #%u \n",
			padded_len, tx_msg->size, num_pls+1);
		d_printf(2, dev,
			 "TX: appended hdr @%zu %zu b pl #%u @%zu %zu/%zu b\n",
			 (void *)tx_msg - i2400m->tx_buf, (size_t)tx_msg->size,
			 num_pls+1, ptr - i2400m->tx_buf, buf_len, padded_len);
		result = 0;
		if (is_singleton)
			i2400m_tx_close(i2400m);
	}
error_tx_new:
	spin_unlock_irqrestore(&i2400m->tx_lock, flags);
	i2400m->bus_tx_kick(i2400m);	/* always kick, might free up space */
	d_fnend(3, dev, "(i2400m %p skb %p [%zu bytes] pt %u) = %d\n",
		i2400m, buf, buf_len, pl_type, result);
	return result;
}
EXPORT_SYMBOL_GPL(i2400m_tx);


/**
 * i2400m_tx_msg_get - Get the first TX message in the FIFO to start sending it
 *
 * @i2400m: device descriptors
 * @bus_size: where to place the size of the TX message
 *
 * Called by the bus-specific driver to get the first TX message at
 * the FIF that is ready for transmission.
 *
 * It sets the state in @i2400m to indicate the bus-specific driver is
 * transfering that message (i2400m->tx_msg_size).
 *
 * Once the transfer is completed, call i2400m_tx_msg_sent().
 *
 * Notes:
 *
 *     The size of the TX message to be transmitted might be smaller than
 *     that of the TX message in the FIFO (in case the header was
 *     shorter). Hence, we copy it in @bus_size, for the bus layer to
 *     use. We keep the message's size in i2400m->tx_msg_size so that
 *     when the bus later is done transferring we know how much to
 *     advance the fifo.
 *
 *     We collect statistics here as all the data is available and we
 *     assume it is going to work [see i2400m_tx_msg_sent()].
 */
struct i2400m_msg_hdr *i2400m_tx_msg_get(struct i2400m *i2400m,
					 size_t *bus_size)
{
	struct device *dev = i2400m_dev(i2400m);
	struct i2400m_msg_hdr *tx_msg, *tx_msg_moved;
	unsigned long flags, pls;

	d_fnstart(3, dev, "(i2400m %p bus_size %p)\n", i2400m, bus_size);
	spin_lock_irqsave(&i2400m->tx_lock, flags);
skip:
	tx_msg_moved = NULL;
	if (i2400m->tx_in == i2400m->tx_out) {	/* Empty FIFO? */
		i2400m->tx_in = 0;
		i2400m->tx_out = 0;
		d_printf(2, dev, "TX: FIFO empty: resetting\n");
		goto out_unlock;
	}
	tx_msg = i2400m->tx_buf + i2400m->tx_out % I2400M_TX_BUF_SIZE;
	if (tx_msg->size & I2400M_TX_SKIP) {	/* skip? */
		d_printf(2, dev, "TX: skip: msg @%zu (%zu b)\n",
			 i2400m->tx_out % I2400M_TX_BUF_SIZE,
			 (size_t) tx_msg->size & ~I2400M_TX_SKIP);
		i2400m->tx_out += tx_msg->size & ~I2400M_TX_SKIP;
		goto skip;
	}

	if (tx_msg->num_pls == 0) {		/* No payloads? */
		if (tx_msg == i2400m->tx_msg) {	/* open, we are done */
			d_printf(2, dev,
				 "TX: FIFO empty: open msg w/o payloads @%zu\n",
				 (void *) tx_msg - i2400m->tx_buf);
			tx_msg = NULL;
			goto out_unlock;
		} else {			/* closed, skip it */
			d_printf(2, dev,
				 "TX: skip msg w/o payloads @%zu (%zu b)\n",
				 (void *) tx_msg - i2400m->tx_buf,
				 (size_t) tx_msg->size);
			i2400m->tx_out += tx_msg->size & ~I2400M_TX_SKIP;
			goto skip;
		}
	}
	if (tx_msg == i2400m->tx_msg)		/* open msg? */
		i2400m_tx_close(i2400m);

	/* Now we have a valid TX message (with payloads) to TX */
	tx_msg_moved = (void *) tx_msg + tx_msg->offset;
	i2400m->tx_msg_size = tx_msg->size;
	*bus_size = tx_msg_moved->size;
	d_printf(2, dev, "TX: pid %d msg hdr at @%zu offset +@%zu "
		 "size %zu bus_size %zu\n",
		 current->pid, (void *) tx_msg - i2400m->tx_buf,
		 (size_t) tx_msg->offset, (size_t) tx_msg->size,
		 (size_t) tx_msg_moved->size);
	tx_msg_moved->barker = le32_to_cpu(I2400M_H2D_PREVIEW_BARKER);
	tx_msg_moved->sequence = le32_to_cpu(i2400m->tx_sequence++);

	pls = le32_to_cpu(tx_msg_moved->num_pls);
	i2400m->tx_pl_num += pls;		/* Update stats */
	if (pls > i2400m->tx_pl_max)
		i2400m->tx_pl_max = pls;
	if (pls < i2400m->tx_pl_min)
		i2400m->tx_pl_min = pls;
	i2400m->tx_num++;
	i2400m->tx_size_acc += *bus_size;
	if (*bus_size < i2400m->tx_size_min)
		i2400m->tx_size_min = *bus_size;
	if (*bus_size > i2400m->tx_size_max)
		i2400m->tx_size_max = *bus_size;
out_unlock:
	spin_unlock_irqrestore(&i2400m->tx_lock, flags);
	d_fnstart(3, dev, "(i2400m %p bus_size %p [%zu]) = %p\n",
		  i2400m, bus_size, *bus_size, tx_msg_moved);
	return tx_msg_moved;
}
EXPORT_SYMBOL_GPL(i2400m_tx_msg_get);


/**
 * i2400m_tx_msg_sent - indicate the transmission of a TX message
 *
 * @i2400m: device descriptor
 *
 * Called by the bus-specific driver when a message has been sent;
 * this pops it from the FIFO; and as there is space, start the queue
 * in case it was stopped.
 *
 * Should be called even if the message send failed and we are
 * dropping this TX message.
 */
void i2400m_tx_msg_sent(struct i2400m *i2400m)
{
	unsigned n;
	unsigned long flags;
	struct device *dev = i2400m_dev(i2400m);

	d_fnstart(3, dev, "(i2400m %p)\n", i2400m);
	spin_lock_irqsave(&i2400m->tx_lock, flags);
	i2400m->tx_out += i2400m->tx_msg_size;
	d_printf(2, dev, "TX: sent %zu b\n", (size_t) i2400m->tx_msg_size);
	i2400m->tx_msg_size = 0;
	BUG_ON(i2400m->tx_out > i2400m->tx_in);
	/* level them FIFO markers off */
	n = i2400m->tx_out / I2400M_TX_BUF_SIZE;
	i2400m->tx_out %= I2400M_TX_BUF_SIZE;
	i2400m->tx_in -= n * I2400M_TX_BUF_SIZE;
	netif_start_queue(i2400m->wimax_dev.net_dev);
	spin_unlock_irqrestore(&i2400m->tx_lock, flags);
	d_fnend(3, dev, "(i2400m %p) = void\n", i2400m);
}
EXPORT_SYMBOL_GPL(i2400m_tx_msg_sent);


/**
 * i2400m_tx_setup - Initialize the TX queue and infrastructure
 *
 * Make sure we reset the TX sequence to zero, as when this function
 * is called, the firmware has been just restarted.
 */
int i2400m_tx_setup(struct i2400m *i2400m)
{
	int result;

	/* Do this here only once -- can't do on
	 * i2400m_hard_start_xmit() as we'll cause race conditions if
	 * the WS was scheduled on another CPU */
	INIT_WORK(&i2400m->wake_tx_ws, i2400m_wake_tx_work);

	i2400m->tx_sequence = 0;
	i2400m->tx_buf = kmalloc(I2400M_TX_BUF_SIZE, GFP_KERNEL);
	if (i2400m->tx_buf == NULL)
		result = -ENOMEM;
	else
		result = 0;
	/* Huh? the bus layer has to define this... */
	BUG_ON(i2400m->bus_tx_block_size == 0);
	return result;

}


/**
 * i2400m_tx_release - Tear down the TX queue and infrastructure
 */
void i2400m_tx_release(struct i2400m *i2400m)
{
	kfree(i2400m->tx_buf);
}
