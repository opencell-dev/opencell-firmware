/* The link task's side of the app lock (review F1, bench 2026-09-30).
 *
 * The exec task gives the app lock away every 50 us while a slot runs and
 * must get it back fast: a link task that validated and copied a SCHEDULE
 * while holding it (up to 728 us on the bench) delayed noticing a full
 * packet's RX done by up to 217 us, against 139 us of slack. A SCHEDULE is
 * now validated and copied out of its message before the lock, and only
 * committed (state checks, two copies) under it.
 *
 * The fake lock runs a hook as the link task gets the lock. Whatever the
 * hook changes, only the critical section can see: the exec task having
 * run first (a frame started), time having passed, or the message being
 * scribbled over (proof that the critical section never reads it). */
#include "unity.h"

#include "exec_fixture.h"
#include "oc_bsr.h"

static oc_bsr_t bsr;
static oc_fwupd_t fw;
static oc_msg_t in, ack;
static oc_exec_part_t part; /* the link task's own buffer */
static uint8_t rxbuf[64];   /* stands in for the framer's decoded buffer */

static struct {
    int      held;
    int      holds;
    int      now_unlocked; /* time reads outside the lock */
    uint64_t now;
    void (*at_lock)(void);
} lk;

static void f_lock(void *ctx)
{
    (void)ctx;
    TEST_ASSERT_FALSE(lk.held);
    lk.held = 1;
    lk.holds++;
    if (lk.at_lock != NULL) {
        lk.at_lock();
    }
}

static void f_unlock(void *ctx)
{
    (void)ctx;
    TEST_ASSERT_TRUE(lk.held);
    lk.held = 0;
}

static uint64_t f_now(void *ctx)
{
    (void)ctx;
    lk.now_unlocked += !lk.held;
    return lk.now;
}

static const oc_bsr_lock_t lock_ops = { NULL, f_lock, f_unlock, f_now };

static int save(void *ctx, const oc_config_t *cfg)
{
    (void)ctx;
    (void)cfg;
    return 0;
}

static int fw_begin(void *c) { (void)c; return 0; }
static int fw_write(void *c, uint32_t o, const uint8_t *d, uint16_t l) { (void)c; (void)o; (void)d; (void)l; return 0; }
static int fw_finish(void *c, uint32_t s) { (void)c; (void)s; return 0; }
static void fw_abort(void *c) { (void)c; }

/* "now" = 10 ms into frame F0 */
#define NOW      (T0 + 10000)
#define F1_START (T0 + OC_FRAME_US)

void setUp(void)
{
    fixture_reset();
    const oc_fwupd_ops_t fops = { NULL, fw_begin, fw_write, fw_finish, fw_abort };
    oc_fwupd_init(&fw, &fops);
    const oc_bsr_ops_t ops = { NULL, save };
    const oc_config_t cfg = { OC_ROLE_BS_RADIO, OC_BAND_915, 0, 7 };
    oc_bsr_init(&bsr, &ops, &clk, &exec_, &fw, &cfg);
    oc_bsr_tick(&bsr, NOW); /* configured, clock locked: TX on */
    memset(&in, 0, sizeof(in));
    memset(&lk, 0, sizeof(lk));
    lk.now = NOW;
    for (unsigned i = 0; i < sizeof(rxbuf); i++) {
        rxbuf[i] = (uint8_t)(i + 1);
    }
}
void tearDown(void) {}

static uint8_t link_send(void)
{
    lk.holds = 0;
    TEST_ASSERT_EQUAL_INT(1, oc_bsr_link_handle(&bsr, &in, &part, &lock_ops, &ack));
    TEST_ASSERT_EQUAL_INT(1, lk.holds);       /* one hold per message */
    TEST_ASSERT_FALSE(lk.held);               /* and given back */
    TEST_ASSERT_EQUAL_INT(0, lk.now_unlocked); /* deadlines judged under the lock */
    TEST_ASSERT_EQUAL_UINT8(OC_MSG_ACK, ack.type);
    TEST_ASSERT_EQUAL_UINT8(in.seq, ack.u.ack.acked_seq);
    return ack.u.ack.status;
}

static void schedule(uint32_t frame, uint8_t flags)
{
    memset(&in, 0, sizeof(in));
    in.type = OC_MSG_SCHEDULE;
    in.seq = 42;
    in.u.schedule.frame_number = frame;
    in.u.schedule.flags = flags;
}

static void add(oc_slot_t s)
{
    in.u.schedule.slots[in.u.schedule.slot_count++] = s;
}

static oc_exec_frame_t *frame_in(uint32_t frame, uint8_t state)
{
    for (unsigned i = 0; i < OC_EXEC_FRAMES; i++) {
        if (exec_.frames[i].state == state && exec_.frames[i].frame_number == frame) {
            return &exec_.frames[i];
        }
    }
    return NULL;
}

static int frame_anywhere(uint32_t frame)
{
    for (unsigned i = 0; i < OC_EXEC_FRAMES; i++) {
        if (exec_.frames[i].state != OC_EXEC_BUF_EMPTY && exec_.frames[i].frame_number == frame) {
            return 1;
        }
    }
    return 0;
}

/* Scribble over the whole SCHEDULE and the bytes its payloads point at. */
static void poison_message(void)
{
    memset(&in.u.schedule, 0xA5, sizeof(in.u.schedule));
    for (unsigned i = 0; i < OC_MAX_SLOTS_PER_SCHEDULE; i++) {
        in.u.schedule.slots[i].payload = rxbuf; /* wrong data if read, not a crash */
    }
    memset(rxbuf, 0x5A, sizeof(rxbuf));
}

static void test_a_schedule_is_validated_and_copied_before_the_lock(void)
{
    schedule(F0 + 1, OC_SCHED_FLAG_FIRST | OC_SCHED_FLAG_LAST);
    add(tx_slot(0, 17000, rxbuf, 28));
    add(rx_slot(20000, 17000));
    lk.at_lock = poison_message;
    TEST_ASSERT_EQUAL_UINT8(OC_ACK_OK, link_send());

    oc_exec_frame_t *b = frame_in(F0 + 1, OC_EXEC_BUF_READY);
    TEST_ASSERT_NOT_NULL(b);
    TEST_ASSERT_EQUAL_UINT8(2, b->slot_count);
    TEST_ASSERT_EQUAL_UINT32(0, b->slots[0].offset_us);
    TEST_ASSERT_EQUAL_UINT32(17000, b->slots[0].length_us);
    TEST_ASSERT_EQUAL_UINT32(915250000u, b->slots[0].freq_hz);
    TEST_ASSERT_EQUAL_UINT8(OC_DIR_TX, b->slots[0].dir);
    TEST_ASSERT_EQUAL_UINT8(28, b->slots[0].payload_len);
    TEST_ASSERT_EQUAL_UINT32(20000, b->slots[1].offset_us);
    TEST_ASSERT_EQUAL_UINT8(OC_DIR_RX, b->slots[1].dir);
    TEST_ASSERT_EQUAL_UINT16(28, b->pool_used);
    for (unsigned i = 0; i < 28; i++) {
        TEST_ASSERT_EQUAL_HEX8(i + 1, b->pool[b->slots[0].payload_off + i]);
    }
}

static void make_message_valid(void)
{
    in.u.schedule.slots[0].length_us = 17000;
}

static void test_a_malformed_part_is_judged_from_its_prepared_copy(void)
{
    schedule(F0 + 1, OC_SCHED_FLAG_FIRST);
    add(rx_slot(0, 17000));
    TEST_ASSERT_EQUAL_UINT8(OC_ACK_OK, link_send());

    schedule(F0 + 1, OC_SCHED_FLAG_LAST);
    add(rx_slot(20000, 0)); /* zero length: malformed */
    lk.at_lock = make_message_valid;
    TEST_ASSERT_EQUAL_UINT8(OC_ACK_ERR_MALFORMED, link_send());
    TEST_ASSERT_FALSE(frame_anywhere(F0 + 1)); /* the half-built frame is dropped, as before */
}

static void time_passes_past_the_deadline(void)
{
    lk.now = F1_START - OC_EXEC_SETUP_US + 1;
}

static void test_the_deadline_is_judged_at_the_commit(void)
{
    schedule(F0 + 1, OC_SCHED_FLAG_FIRST | OC_SCHED_FLAG_LAST);
    add(rx_slot(0, 17000));
    lk.at_lock = time_passes_past_the_deadline;
    TEST_ASSERT_EQUAL_UINT8(OC_ACK_ERR_LATE, link_send());
    TEST_ASSERT_FALSE(frame_anywhere(F0 + 1));
}

/* The exec task had the lock first and entered F0+1 (a configure-lead
 * early): its first slot is configured and staged from the READY buffer. */
static void exec_enters_the_frame(void)
{
    run_until(NOW, F1_START - 1000);
    lk.now = F1_START - 1000;
}

static void test_a_frame_that_started_running_before_the_commit_is_left_intact(void)
{
    schedule(F0 + 1, OC_SCHED_FLAG_FIRST | OC_SCHED_FLAG_LAST);
    add(tx_slot(0, 17000, rxbuf, 28));
    add(rx_slot(20000, 17000));
    TEST_ASSERT_EQUAL_UINT8(OC_ACK_OK, link_send());

    /* The host re-sends the frame differently; the exec task starts it
     * between the link task's prepare and its commit. */
    schedule(F0 + 1, OC_SCHED_FLAG_FIRST | OC_SCHED_FLAG_LAST);
    add(rx_slot(50000, 17000));
    lk.at_lock = exec_enters_the_frame;
    TEST_ASSERT_EQUAL_UINT8(OC_ACK_ERR_LATE, link_send());

    TEST_ASSERT_NOT_NULL(exec_.run);
    TEST_ASSERT_EQUAL_UINT8(OC_EXEC_BUF_RUNNING, exec_.run->state);
    TEST_ASSERT_EQUAL_UINT8(2, exec_.run->slot_count);
    TEST_ASSERT_EQUAL_UINT32(0, exec_.run->slots[0].offset_us);

    run_until(F1_START - 1000, F1_START + 40000);
    TEST_ASSERT_EQUAL_INT(2, count_calls(CALL_LAUNCH)); /* the original two slots, nothing else */
    TEST_ASSERT_EQUAL_UINT64(F1_START, fake.calls[find_call(CALL_LAUNCH, 0)].target_us);
    TEST_ASSERT_EQUAL_UINT64(F1_START + 20000, fake.calls[find_call(CALL_LAUNCH, 1)].target_us);
    TEST_ASSERT_EQUAL_UINT32(28, fake.calls[find_call(CALL_STAGE_TX, 0)].arg);
}

static void test_the_link_buffer_is_reused_without_touching_committed_frames(void)
{
    schedule(F0 + 1, OC_SCHED_FLAG_FIRST | OC_SCHED_FLAG_LAST);
    add(tx_slot(0, 17000, rxbuf, 28));
    TEST_ASSERT_EQUAL_UINT8(OC_ACK_OK, link_send());

    /* The link task reuses its buffer for the next message at once. */
    memset(&part, 0xEE, sizeof(part));
    memset(rxbuf, 0x33, sizeof(rxbuf));
    schedule(F0 + 2, OC_SCHED_FLAG_FIRST | OC_SCHED_FLAG_LAST);
    add(tx_slot(5000, 17000, rxbuf, 20));
    add(rx_slot(30000, 17000));
    TEST_ASSERT_EQUAL_UINT8(OC_ACK_OK, link_send());

    oc_exec_frame_t *b = frame_in(F0 + 1, OC_EXEC_BUF_READY);
    TEST_ASSERT_NOT_NULL(b);
    TEST_ASSERT_EQUAL_UINT8(1, b->slot_count);
    TEST_ASSERT_EQUAL_UINT32(0, b->slots[0].offset_us);
    TEST_ASSERT_EQUAL_UINT8(28, b->slots[0].payload_len);
    for (unsigned i = 0; i < 28; i++) {
        TEST_ASSERT_EQUAL_HEX8(i + 1, b->pool[b->slots[0].payload_off + i]);
    }
    oc_exec_frame_t *c = frame_in(F0 + 2, OC_EXEC_BUF_READY);
    TEST_ASSERT_NOT_NULL(c);
    TEST_ASSERT_EQUAL_UINT8(2, c->slot_count);
    TEST_ASSERT_EQUAL_HEX8(0x33, c->pool[c->slots[0].payload_off + 19]);

    run_until(NOW, F1_START + 30000);
    TEST_ASSERT_EQUAL_UINT64(F1_START, fake.calls[find_call(CALL_LAUNCH, 0)].target_us);
    TEST_ASSERT_EQUAL_UINT32(28, fake.calls[find_call(CALL_STAGE_TX, 0)].arg);
}

static void test_every_message_type_is_one_hold(void)
{
    in.type = OC_MSG_TIME;
    in.seq = 1;
    in.u.time.unix_s = UTC0;
    TEST_ASSERT_EQUAL_UINT8(OC_ACK_OK, link_send());

    static uint8_t img[16];
    memset(&in, 0, sizeof(in));
    in.type = OC_MSG_FW_CHUNK;
    in.seq = 2;
    in.u.fw_chunk = (oc_fw_chunk_t){ 0, sizeof(img), img };
    TEST_ASSERT_EQUAL_UINT8(OC_ACK_OK, link_send());

    memset(&in, 0, sizeof(in));
    in.type = OC_MSG_CONFIG;
    in.seq = 3;
    in.u.config = (oc_config_t){ OC_ROLE_BS_RADIO, OC_BAND_915, 1, 7 };
    TEST_ASSERT_EQUAL_UINT8(OC_ACK_OK, link_send());
    TEST_ASSERT_EQUAL_UINT8(1, bsr.config.radio_index);

    schedule(F0 + 1, OC_SCHED_FLAG_FIRST | OC_SCHED_FLAG_LAST);
    add(rx_slot(0, 17000));
    TEST_ASSERT_EQUAL_UINT8(OC_ACK_OK, link_send());
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_a_schedule_is_validated_and_copied_before_the_lock);
    RUN_TEST(test_a_malformed_part_is_judged_from_its_prepared_copy);
    RUN_TEST(test_the_deadline_is_judged_at_the_commit);
    RUN_TEST(test_a_frame_that_started_running_before_the_commit_is_left_intact);
    RUN_TEST(test_the_link_buffer_is_reused_without_touching_committed_frames);
    RUN_TEST(test_every_message_type_is_one_hold);
    return UNITY_END();
}
