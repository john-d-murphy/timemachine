/*
 *  Ring buffer unit tests for timemachine.
 *  No JACK, GTK, or libsndfile required — pure C math.
 *
 *  Build:  gcc -O2 -Wall -o test_ring test_ring.c -lm
 *  Run:    ./test_ring
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- minimal test harness ---- */

static int tests_run = 0;
static int tests_passed = 0;
static int tests_failed = 0;

#define ASSERT(cond, msg)                                       \
  do {                                                          \
    tests_run++;                                                \
    if (!(cond)) {                                              \
      fprintf(stderr, "  FAIL: %s (line %d)\n", msg, __LINE__); \
      tests_failed++;                                           \
    } else {                                                    \
      tests_passed++;                                           \
    }                                                           \
  } while (0)

#define ASSERT_EQ_FLOAT(a, b, msg) ASSERT(fabsf((a) - (b)) < 1e-6f, msg)

#define TEST(name) static void name(void)
#define RUN(name)            \
  do {                       \
    printf("  %s\n", #name); \
    name();                  \
  } while (0)

/* ---- extracted ring buffer logic (mirrors threads.c) ---- */

#define MAX_PORTS 8

typedef struct {
  float* ring[MAX_PORTS];
  unsigned int ring_size;
  unsigned int ring_head; /* monotonic */
  unsigned int num_ports;
} ring_state_t;

static void ring_init(ring_state_t* rs, unsigned int size, unsigned int ports) {
  rs->ring_size = size;
  rs->ring_head = 0;
  rs->num_ports = ports;
  for (unsigned int p = 0; p < ports; p++) {
    rs->ring[p] = calloc(size, sizeof(float));
  }
  for (unsigned int p = ports; p < MAX_PORTS; p++) {
    rs->ring[p] = NULL;
  }
}

static void ring_free(ring_state_t* rs) {
  for (unsigned int p = 0; p < rs->num_ports; p++) {
    free(rs->ring[p]);
    rs->ring[p] = NULL;
  }
}

/* simulate process() — write nframes of data per port */
static void ring_write(ring_state_t* rs, float** in, unsigned int nframes) {
  unsigned int pos = rs->ring_head;
  for (unsigned int port = 0; port < rs->num_ports; port++) {
    for (unsigned int i = 0; i < nframes; i++) {
      rs->ring[port][(pos + i) % rs->ring_size] = in[port][i];
    }
  }
  rs->ring_head = pos + nframes;
}

/* simulate the capture read path from writer_thread */
static void ring_capture(ring_state_t* rs, unsigned int capture_seconds,
                         unsigned int sr, float* out_buf,
                         unsigned int* out_frames) {
  unsigned int head = rs->ring_head;
  unsigned int frames = capture_seconds * sr;

  /* clamp to ring size */
  if (frames > rs->ring_size) {
    frames = rs->ring_size;
  }
  /* clamp to available data */
  if (frames > head) {
    frames = head;
  }

  unsigned int start = head - frames;

  /* interleave into output buffer */
  for (unsigned int i = 0; i < frames; i++) {
    for (unsigned int port = 0; port < rs->num_ports; port++) {
      out_buf[i * rs->num_ports + port] =
          rs->ring[port][(start + i) % rs->ring_size];
    }
  }

  *out_frames = frames;
}

/* ---- tests ---- */

TEST(test_basic_write_read) {
  /* write 10 samples into a 32-sample ring, capture them back */
  ring_state_t rs;
  ring_init(&rs, 32, 1);

  float input[10];
  for (int i = 0; i < 10; i++) input[i] = (float)(i + 1);
  float* in_ptrs[1] = {input};

  ring_write(&rs, in_ptrs, 10);

  ASSERT(rs.ring_head == 10, "head should be 10 after writing 10 frames");

  /* capture all 10 — pretend sr=1 so seconds=frames */
  float out[32];
  unsigned int got;
  ring_capture(&rs, 10, 1, out, &got);

  ASSERT(got == 10, "should capture 10 frames");
  for (int i = 0; i < 10; i++) {
    ASSERT_EQ_FLOAT(out[i], (float)(i + 1), "captured data should match input");
  }

  ring_free(&rs);
}

TEST(test_wrap_around) {
  /* ring of 8, write 20 samples — should wrap and overwrite */
  ring_state_t rs;
  ring_init(&rs, 8, 1);

  float input[20];
  for (int i = 0; i < 20; i++) input[i] = (float)(i + 1);
  float* in_ptrs[1] = {input};

  ring_write(&rs, in_ptrs, 20);

  ASSERT(rs.ring_head == 20, "head should be 20 (monotonic)");

  /* capture last 8 samples (full ring) */
  float out[8];
  unsigned int got;
  ring_capture(&rs, 8, 1, out, &got);

  ASSERT(got == 8, "should capture 8 frames (ring size)");
  /* last 8 values written were 13..20 */
  for (int i = 0; i < 8; i++) {
    ASSERT_EQ_FLOAT(out[i], (float)(13 + i), "should get last 8 samples");
  }

  ring_free(&rs);
}

TEST(test_partial_capture) {
  /* write 100 samples, capture only last 10 */
  ring_state_t rs;
  ring_init(&rs, 64, 1);

  float input[100];
  for (int i = 0; i < 100; i++) input[i] = (float)(i + 1);

  /* write in chunks like JACK would */
  for (int chunk = 0; chunk < 100; chunk += 16) {
    float* ptr = input + chunk;
    float* ptrs[1] = {ptr};
    int n = (chunk + 16 <= 100) ? 16 : 100 - chunk;
    ring_write(&rs, ptrs, n);
  }

  float out[64];
  unsigned int got;
  ring_capture(&rs, 10, 1, out, &got);

  ASSERT(got == 10, "should capture 10 frames");
  /* last 10 values: 91..100 */
  for (int i = 0; i < 10; i++) {
    ASSERT_EQ_FLOAT(out[i], (float)(91 + i), "partial capture should get tail");
  }

  ring_free(&rs);
}

TEST(test_clamp_to_available) {
  /* ring of 64, only 5 frames written, ask for 20 */
  ring_state_t rs;
  ring_init(&rs, 64, 1);

  float input[5] = {1.0f, 2.0f, 3.0f, 4.0f, 5.0f};
  float* in_ptrs[1] = {input};
  ring_write(&rs, in_ptrs, 5);

  float out[64];
  unsigned int got;
  ring_capture(&rs, 20, 1, out, &got);

  ASSERT(got == 5, "should clamp to available (5 frames)");
  for (int i = 0; i < 5; i++) {
    ASSERT_EQ_FLOAT(out[i], (float)(i + 1), "clamped capture data correct");
  }

  ring_free(&rs);
}

TEST(test_clamp_to_ring_size) {
  /* ring of 16, 100 frames written, ask for 50 */
  ring_state_t rs;
  ring_init(&rs, 16, 1);

  float input[100];
  for (int i = 0; i < 100; i++) input[i] = (float)(i + 1);
  float* in_ptrs[1] = {input};
  ring_write(&rs, in_ptrs, 100);

  float out[64];
  unsigned int got;
  ring_capture(&rs, 50, 1, out, &got);

  ASSERT(got == 16, "should clamp to ring_size (16)");
  /* last 16: 85..100 */
  for (int i = 0; i < 16; i++) {
    ASSERT_EQ_FLOAT(out[i], (float)(85 + i), "ring-clamped data correct");
  }

  ring_free(&rs);
}

TEST(test_multi_port_interleave) {
  /* 2 ports, verify interleaving */
  ring_state_t rs;
  ring_init(&rs, 32, 2);

  float in_l[4] = {1.0f, 2.0f, 3.0f, 4.0f};
  float in_r[4] = {10.0f, 20.0f, 30.0f, 40.0f};
  float* in_ptrs[2] = {in_l, in_r};
  ring_write(&rs, in_ptrs, 4);

  float out[64];
  unsigned int got;
  ring_capture(&rs, 4, 1, out, &got);

  ASSERT(got == 4, "should capture 4 frames");
  /* interleaved: L R L R L R L R */
  ASSERT_EQ_FLOAT(out[0], 1.0f, "frame 0 left");
  ASSERT_EQ_FLOAT(out[1], 10.0f, "frame 0 right");
  ASSERT_EQ_FLOAT(out[2], 2.0f, "frame 1 left");
  ASSERT_EQ_FLOAT(out[3], 20.0f, "frame 1 right");
  ASSERT_EQ_FLOAT(out[4], 3.0f, "frame 2 left");
  ASSERT_EQ_FLOAT(out[5], 30.0f, "frame 2 right");
  ASSERT_EQ_FLOAT(out[6], 4.0f, "frame 3 left");
  ASSERT_EQ_FLOAT(out[7], 40.0f, "frame 3 right");

  ring_free(&rs);
}

TEST(test_nondestructive_capture) {
  /* capture should not modify the ring — capture twice, get same data */
  ring_state_t rs;
  ring_init(&rs, 32, 1);

  float input[10];
  for (int i = 0; i < 10; i++) input[i] = (float)(i + 1);
  float* in_ptrs[1] = {input};
  ring_write(&rs, in_ptrs, 10);

  float out1[32], out2[32];
  unsigned int got1, got2;

  ring_capture(&rs, 10, 1, out1, &got1);
  ring_capture(&rs, 10, 1, out2, &got2);

  ASSERT(got1 == got2, "both captures should return same count");
  ASSERT(rs.ring_head == 10, "ring_head unchanged after capture");
  for (int i = 0; i < 10; i++) {
    ASSERT_EQ_FLOAT(out1[i], out2[i], "repeated capture gives same data");
  }

  ring_free(&rs);
}

TEST(test_overlapping_captures) {
  /* write 20, capture last 10, write 5 more, capture last 10 again —
     second capture should overlap with first but include new data */
  ring_state_t rs;
  ring_init(&rs, 32, 1);

  float input[25];
  for (int i = 0; i < 25; i++) input[i] = (float)(i + 1);

  float* p1 = input;
  float* ptrs1[1] = {p1};
  ring_write(&rs, ptrs1, 20);

  float out1[32];
  unsigned int got1;
  ring_capture(&rs, 10, 1, out1, &got1);
  ASSERT(got1 == 10, "first capture: 10 frames");
  ASSERT_EQ_FLOAT(out1[0], 11.0f, "first capture starts at 11");

  float* p2 = input + 20;
  float* ptrs2[1] = {p2};
  ring_write(&rs, ptrs2, 5);

  float out2[32];
  unsigned int got2;
  ring_capture(&rs, 10, 1, out2, &got2);
  ASSERT(got2 == 10, "second capture: 10 frames");
  ASSERT_EQ_FLOAT(out2[0], 16.0f, "second capture starts at 16");
  ASSERT_EQ_FLOAT(out2[9], 25.0f, "second capture ends at 25");

  ring_free(&rs);
}

TEST(test_sample_rate_scaling) {
  /* simulate sr=48000, buf of 48000 frames (1 sec), capture 1 second */
  unsigned int sr = 48000;
  ring_state_t rs;
  ring_init(&rs, sr, 1);

  /* fill the whole buffer */
  float* input = calloc(sr, sizeof(float));
  for (unsigned int i = 0; i < sr; i++) input[i] = (float)i;
  float* in_ptrs[1] = {input};
  ring_write(&rs, in_ptrs, sr);

  float* out = calloc(sr, sizeof(float));
  unsigned int got;
  ring_capture(&rs, 1, sr, out, &got);

  ASSERT(got == sr, "1 second at 48kHz = 48000 frames");
  ASSERT_EQ_FLOAT(out[0], 0.0f, "first sample");
  ASSERT_EQ_FLOAT(out[sr - 1], (float)(sr - 1), "last sample");

  free(input);
  free(out);
  ring_free(&rs);
}

TEST(test_empty_ring) {
  /* capture from empty ring should return 0 frames */
  ring_state_t rs;
  ring_init(&rs, 32, 1);

  float out[32];
  unsigned int got;
  ring_capture(&rs, 10, 1, out, &got);

  ASSERT(got == 0, "empty ring should yield 0 frames");

  ring_free(&rs);
}

/* ---- main ---- */

int main(void) {
  printf("timemachine ring buffer tests\n");
  printf("-----------------------------\n");

  RUN(test_basic_write_read);
  RUN(test_wrap_around);
  RUN(test_partial_capture);
  RUN(test_clamp_to_available);
  RUN(test_clamp_to_ring_size);
  RUN(test_multi_port_interleave);
  RUN(test_nondestructive_capture);
  RUN(test_overlapping_captures);
  RUN(test_sample_rate_scaling);
  RUN(test_empty_ring);

  printf("-----------------------------\n");
  printf("%d tests, %d passed, %d failed\n", tests_run, tests_passed,
         tests_failed);

  return tests_failed ? 1 : 0;
}
