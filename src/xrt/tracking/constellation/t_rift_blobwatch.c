// Copyright 2014-2015, Philipp Zabel
// Copyright 2019-2023, Jan Schmidt
// Copyright 2025-2026, Beyley Cardellio
// SPDX-License-Identifier:	BSL-1.0
/*!
 * @file
 * @brief  Blob thresholding and tracking in camera frames
 * @author Philipp Zabel <philipp.zabel@gmail.com>
 * @author Jan Schmidt <jan@centricular.com>
 * @author Beyley Cardellio <ep1cm1n10n123@gmail.com>
 * @ingroup tracking
 */

#include "xrt/xrt_defines.h"
#include "xrt/xrt_frame.h"
#include "xrt/xrt_config_build.h"

#include "util/u_logging.h"
#include "util/u_misc.h"
#include "util/u_frame.h"
#include "util/u_time.h"
#include "util/u_var.h"

#include "os/os_threading.h"

#include "tracking/t_constellation.h"
#include "tracking/t_camera_models.h"

#include "math/m_api.h"

#include "t_rift_blobwatch.h"

#include <string.h>


typedef uint16_t led_id_t;

// @todo Remove when clang-format is updated in CI
// clang-format off
#define LED_INVALID_ID ((led_id_t)(-1))
#define LED_NOISE_ID ((led_id_t)(-2))
#define LED_LOCAL_ID(l) (((l) == LED_INVALID_ID) ? (l) : (l) & 0xFF)
#define LED_OBJECT_ID(l) (((l) == LED_INVALID_ID) ? (l) : (l) >> 8)
#define LED_MAKE_ID(o, n) ((led_id_t)(((led_id_t)(o)) << 8 | ((led_id_t)(n))))
// clang-format on

struct blob
{
	/*
	 * Each new blob is assigned a unique ID and used to match between frames when updating blob labels from a
	 * delayed long-analysis. 4 billion ought to be enough before it wraps
	 */
	uint32_t blob_id;

	// Weighted greysum centre of blob
	float x;
	float y;

	// Motion vector from previous blob
	float vx;
	float vy;

	// The greysum of the blob, over it's whole area
	uint32_t greysum;

	// bounding box
	uint16_t top;
	uint16_t left;

	uint16_t width;
	uint16_t height;
	uint32_t area;
	uint32_t age;
	int16_t track_index;

	uint32_t id_age;
	led_id_t led_id;
	led_id_t prev_led_id;
};

/*
 * Stores all blobs observed in a single frame.
 */
struct blobservation
{
	uint32_t num_blobs;
	struct blob blobs[XRT_CONSTELLATION_MAX_BLOBS_PER_FRAME];
	uint8_t tracked[XRT_CONSTELLATION_MAX_BLOBS_PER_FRAME];

	uint32_t dropped_dark_blobs;

	timepoint_ns timestamp_ns;
};

/*
 * Keep enough history frames that the caller can keep hold
 * of a previous struct blobservation and pass it to the long term
 * tracker while we still have 2 left to ping-pong between
 */
#define NUM_FRAMES_HISTORY 5
#define MAX_EXTENTS_PER_LINE 30

// Set to 1 to do extra array tracking consistency checks
#define CONSISTENCY_CHECKS 0

#define QUEUE_ENTRIES (NUM_FRAMES_HISTORY + 1)

/*
 * A dimmer peak in a blob is split off as its own LED if the brightest path to a brighter peak dips below this
 * fraction of its brightness, both measured from the pixel threshold.
 */
#define BLOB_SPLIT_SADDLE_RATIO 0.75f
// Blobs with more peaks than this, such as large noisy areas, are not split.
#define MAX_PEAKS_PER_BLOB 32
#define PEAK_NONE ((uint8_t)0xFF)

struct extent
{
	uint16_t start;
	uint16_t end;
	// inherited parameters
	uint16_t top;
	uint16_t left;
	uint16_t right;
	uint32_t area;
	// x of the first pixel on the top line, a pixel to flood fill the whole blob from
	uint16_t seed_x;

	// Maximum pixel colour detected
	uint8_t max_pixel;
};

struct blob_pixel
{
	uint16_t x;
	uint16_t y;
	uint8_t value;
	// Index of the peak this pixel belongs to, PEAK_NONE until assigned
	uint8_t peak;
};

struct blob_bounds
{
	uint16_t left;
	uint16_t top;
	uint16_t right;
	uint16_t bottom;
};

struct extent_line
{
	struct extent extents[MAX_EXTENTS_PER_LINE];
	uint16_t num;
	uint16_t padding[3];
};

struct blobservation_queue
{
	struct blobservation *data[QUEUE_ENTRIES];
	uint32_t head, tail;
};

static void
init_queue(struct blobservation_queue *q)
{
	q->head = q->tail = 0;
}

static void
push_queue(struct blobservation_queue *q, struct blobservation *b)
{
	uint32_t next = (q->tail + 1) % QUEUE_ENTRIES;
	assert(next != q->head); // Check there's room
	assert(b != NULL);
	q->data[q->tail] = b;
	q->tail = next;
}

static struct blobservation *
pop_queue(struct blobservation_queue *q)
{
	struct blobservation *b;
	uint32_t next_head = (q->head + 1) % QUEUE_ENTRIES;

	if ((q)->tail == (q)->head) { // Check there's something in the queue
		return NULL;
	}

	b = q->data[q->head];
	q->head = next_head;

	return b;
}

/*
 * Blob detector internal state
 */
struct t_rift_blobwatch
{
	struct t_blobwatch base;

	struct xrt_frame_node node;
	struct xrt_frame_sink frame_sink;

	struct os_mutex mutex;

	struct t_blob_sink *target_sink;

	uint32_t next_blob_id;
	struct t_rift_blobwatch_params params;
	struct t_camera_model_params camera_model_params;

	/*!
	 * Cached square of the maximum distance for matching a blob between frames,
	 * to avoid sqrt in a hot path.
	 */
	float max_match_dist_sq;

	struct blobservation observations[NUM_FRAMES_HISTORY];

	struct blobservation_queue observation_q;

	struct blobservation *last_observation;

	// Scratch buffers for collecting the connected pixels of each blob
	struct
	{
		// Frame sized, the generation each pixel was last visited in, so a blob is only collected once a frame
		uint8_t *visited;
		uint8_t generation;
		uint32_t width;
		uint32_t height;

		// Flood fill stack of packed pixel coordinates, only grows past the largest allowed blob for larger
		// ones
		uint32_t *stack;
		uint32_t stack_capacity;

		// Sized for the largest allowed blob, its pixels, their indices by position, and their brightness order
		struct blob_pixel *pixels;
		uint32_t *pixel_index;
		uint32_t *order;
		uint32_t capacity;

		// The maximum blob width the buffers are sized for, the parameter can change while processing a frame
		uint16_t max_blob_width;
	} scratch;

	struct
	{
		struct u_var_draggable_u8 blob_required_threshold;
		struct u_var_draggable_u8 pixel_threshold;
	} gui;
};

static inline struct t_rift_blobwatch *
t_rift_blobwatch(struct t_blobwatch *bw)
{
	return (struct t_rift_blobwatch *)bw;
}

/*
 * Flood fills the pixels connected to the seed into the scratch pixels, marking them visited so other extents of the
 * same blob skip it. Returns false if the blob is wider or taller than the maximum blob width, the whole blob is still
 * marked visited so none of its parts get stored.
 *
 * Also counts the bright enough local maxima, ties going to the earlier pixel. Every separate peak a blob could be
 * split at contains one, so a blob with fewer than two can't be split.
 */
static bool
collect_blob_pixels(struct t_rift_blobwatch *bw,
                    struct xrt_frame *frame,
                    uint16_t seed_x,
                    uint16_t seed_y,
                    uint32_t *out_count,
                    uint32_t *out_peak_count,
                    struct blob_bounds *out_bounds)
{
	uint8_t *visited = bw->scratch.visited;
	const uint8_t generation = bw->scratch.generation;
	const uint32_t width = frame->width;
	const uint32_t height = frame->height;
	const uint8_t threshold = bw->params.pixel_threshold;
	const uint16_t max_width = bw->scratch.max_blob_width;

	struct blob_bounds bounds = {seed_x, seed_y, seed_x, seed_y};
	uint32_t count = 0;
	uint32_t peak_count = 0;
	bool fits = true;

	uint32_t stack_size = 0;
	bw->scratch.stack[stack_size++] = (uint32_t)seed_y << 16 | seed_x;
	visited[seed_y * width + seed_x] = generation;

	while (stack_size > 0) {
		const uint32_t packed = bw->scratch.stack[--stack_size];
		const uint16_t x = packed & 0xFFFF;
		const uint16_t y = packed >> 16;
		const uint8_t value = frame->data[y * frame->stride + x];

		if (fits) {
			bounds.left = MIN(bounds.left, x);
			bounds.top = MIN(bounds.top, y);
			bounds.right = MAX(bounds.right, x);
			bounds.bottom = MAX(bounds.bottom, y);

			if (bounds.right - bounds.left > max_width || bounds.bottom - bounds.top > max_width ||
			    count == bw->scratch.capacity) {
				fits = false;
			} else {
				bw->scratch.pixels[count++] = (struct blob_pixel){
				    .x = x,
				    .y = y,
				    .value = value,
				    .peak = 0,
				};
			}
		}

		// Keep marking the rest of a blob that doesn't fit, so its other extents don't collect a part of it
		const int32_t neighbour_x[4] = {x - 1, x + 1, x, x};
		const int32_t neighbour_y[4] = {y, y, y - 1, y + 1};
		const bool neighbour_earlier[4] = {true, false, true, false};
		bool is_peak = value >= bw->params.blob_required_threshold;
		for (int i = 0; i < 4; i++) {
			const int32_t nx = neighbour_x[i];
			const int32_t ny = neighbour_y[i];
			if (nx < 0 || ny < 0 || nx >= (int32_t)width || ny >= (int32_t)height) {
				continue;
			}

			const uint8_t neighbour_value = frame->data[ny * frame->stride + nx];
			if (neighbour_value > value || (neighbour_value == value && neighbour_earlier[i])) {
				is_peak = false;
			}

			const uint32_t neighbour = ny * width + nx;
			if (visited[neighbour] == generation || neighbour_value <= threshold) {
				continue;
			}

			if (stack_size == bw->scratch.stack_capacity) {
				bw->scratch.stack_capacity = MAX(bw->scratch.stack_capacity * 2, 1);
				U_ARRAY_REALLOC_OR_FREE(bw->scratch.stack, uint32_t, bw->scratch.stack_capacity);
			}

			visited[neighbour] = generation;
			bw->scratch.stack[stack_size++] = (uint32_t)ny << 16 | nx;
		}

		if (is_peak) {
			peak_count++;
		}
	}

	*out_count = count;
	*out_peak_count = peak_count;
	*out_bounds = bounds;

	return fits;
}

static uint8_t
find_peak_root(const uint8_t *parent, uint8_t peak)
{
	while (parent[peak] != peak) {
		peak = parent[peak];
	}

	return peak;
}

/*
 * Assigns each scratch pixel of a blob to an LED peak, by visiting the pixels brightest first and merging the peaks
 * they connect. A dimmer peak is kept as its own LED when the pixel connecting it to a brighter one, the saddle
 * between them, is much darker than it. Each pixel goes to the peak its brightest neighbour leads to. Returns the
 * number of peaks, the pixels' peak indices are compacted to that range.
 */
static uint32_t
split_blob_peaks(struct t_rift_blobwatch *bw, uint32_t count, const struct blob_bounds *bounds)
{
	struct blob_pixel *pixels = bw->scratch.pixels;
	uint32_t *pixel_index = bw->scratch.pixel_index;
	uint32_t *order = bw->scratch.order;
	const uint32_t bounds_width = bounds->right - bounds->left + 1;
	const uint32_t bounds_height = bounds->bottom - bounds->top + 1;
	const float threshold = bw->params.pixel_threshold;

	for (uint32_t i = 0; i < bounds_width * bounds_height; i++) {
		pixel_index[i] = UINT32_MAX;
	}
	for (uint32_t i = 0; i < count; i++) {
		pixel_index[(pixels[i].y - bounds->top) * bounds_width + (pixels[i].x - bounds->left)] = i;
		pixels[i].peak = PEAK_NONE;
	}

	// Counting sort, brightest first, equal pixels stay in flood order
	uint32_t histogram[256] = {0};
	for (uint32_t i = 0; i < count; i++) {
		histogram[pixels[i].value]++;
	}
	uint32_t start = 0;
	for (int value = 255; value >= 0; value--) {
		const uint32_t value_count = histogram[value];
		histogram[value] = start;
		start += value_count;
	}
	for (uint32_t i = 0; i < count; i++) {
		order[histogram[pixels[i].value]++] = i;
	}

	uint8_t parent[MAX_PEAKS_PER_BLOB];
	uint8_t peak_value[MAX_PEAKS_PER_BLOB];
	bool kept[MAX_PEAKS_PER_BLOB] = {0};
	uint32_t peak_count = 0;

	for (uint32_t n = 0; n < count; n++) {
		struct blob_pixel *p = &pixels[order[n]];

		uint32_t neighbours[4];
		uint32_t neighbour_count = 0;
		const int32_t neighbour_x[4] = {p->x - 1, p->x + 1, p->x, p->x};
		const int32_t neighbour_y[4] = {p->y, p->y, p->y - 1, p->y + 1};
		for (int i = 0; i < 4; i++) {
			const int32_t bx = neighbour_x[i] - bounds->left;
			const int32_t by = neighbour_y[i] - bounds->top;
			if (bx < 0 || by < 0 || bx >= (int32_t)bounds_width || by >= (int32_t)bounds_height) {
				continue;
			}

			const uint32_t index = pixel_index[by * bounds_width + bx];
			if (index == UINT32_MAX || pixels[index].peak == PEAK_NONE) {
				continue;
			}

			neighbours[neighbour_count++] = index;
		}

		// No brighter neighbours, this is a new peak
		if (neighbour_count == 0) {
			if (peak_count == MAX_PEAKS_PER_BLOB) {
				// Too many to tell apart, keep the blob whole
				for (uint32_t i = 0; i < count; i++) {
					pixels[i].peak = 0;
				}
				return 1;
			}

			parent[peak_count] = peak_count;
			peak_value[peak_count] = p->value;
			p->peak = peak_count++;
			continue;
		}

		uint32_t brightest = neighbours[0];
		for (uint32_t i = 1; i < neighbour_count; i++) {
			if (pixels[neighbours[i]].value > pixels[brightest].value) {
				brightest = neighbours[i];
			}
		}
		p->peak = pixels[brightest].peak;

		// This pixel is the saddle between any peaks its neighbours lead to, merge the dimmer into the brighter
		uint8_t root = find_peak_root(parent, p->peak);
		for (uint32_t i = 0; i < neighbour_count; i++) {
			uint8_t other = find_peak_root(parent, pixels[neighbours[i]].peak);
			if (other == root) {
				continue;
			}

			if (peak_value[other] > peak_value[root]) {
				const uint8_t tmp = root;
				root = other;
				other = tmp;
			}

			if (peak_value[other] >= bw->params.blob_required_threshold &&
			    (p->value - threshold) <= BLOB_SPLIT_SADDLE_RATIO * (peak_value[other] - threshold)) {
				kept[other] = true;
			}
			parent[other] = root;
		}
	}

	// The brightest peak is always kept
	kept[find_peak_root(parent, 0)] = true;

	uint8_t kept_index[MAX_PEAKS_PER_BLOB];
	uint32_t kept_count = 0;
	for (uint32_t i = 0; i < peak_count; i++) {
		if (kept[i]) {
			kept_index[i] = kept_count++;
		}
	}

	// Merged peaks go to the closest kept peak above them
	for (uint32_t i = 0; i < count; i++) {
		uint8_t peak = pixels[i].peak;
		while (!kept[peak]) {
			peak = parent[peak];
		}
		pixels[i].peak = kept_index[peak];
	}

	return kept_count;
}

/*
 * Stores the scratch pixels of the given peak as a blob, at the weighted greysum centre of the pixels.
 */
static void
store_blob(struct t_rift_blobwatch *bw, struct blobservation *ob, uint32_t count, uint8_t peak)
{
	const struct blob_pixel *pixels = bw->scratch.pixels;
	const uint8_t threshold = bw->params.pixel_threshold;

	struct blob_bounds bounds = {UINT16_MAX, UINT16_MAX, 0, 0};
	uint32_t area = 0;
	uint32_t greysum_total = 0, greysum_x = 0, greysum_y = 0;

	for (uint32_t i = 0; i < count; i++) {
		const struct blob_pixel *p = &pixels[i];
		if (p->peak != peak) {
			continue;
		}

		bounds.left = MIN(bounds.left, p->x);
		bounds.top = MIN(bounds.top, p->y);
		bounds.right = MAX(bounds.right, p->x);
		bounds.bottom = MAX(bounds.bottom, p->y);
		area++;

		// Subtract by the pixel threshold, clamping at 0 if it's below the threshold.
		// @note The reason we do this is so that the greysum is weighted by the values above our noise
		//       floor, as we know all values passed into this function are going to be above it, which
		//       means without this we would naturally be weighted towards the geometric center rather
		//       than the true greysum of the observable values.
		const uint32_t pix = p->value - MIN(p->value, threshold);

		// Use 1...frame_width/height as coords, otherwise the first column never contributes anything
		greysum_total += pix;
		greysum_x += (p->x + 1) * pix;
		greysum_y += (p->y + 1) * pix;
	}

	// Don't store 1x1 blobs
	if (area <= 1) {
		return;
	}

	struct blob *b = &ob->blobs[ob->num_blobs++];
	b->blob_id = bw->next_blob_id++;

	if (greysum_total == 0) {
		// Fallback to geometric center to avoid NaN from division by zero
		b->x = (bounds.left + bounds.right) / 2.0f;
		b->y = (bounds.top + bounds.bottom) / 2.0f;
	} else {
		// @note We don't try to "center" onto the pixel because in OpenCV distortion parameters integer
		//       coordinates already refer to the correct real-world ray.
		//       It's hard to measure this real-world (it's like well under 1% of the measurements), but it
		//       seems likely that this is the case for Rift as well since it uses OpenCV distortion algorithms.

		// Subtract 1 to convert from 1-based to 0-based coordinates
		b->x = ((float)(greysum_x) / greysum_total - 1);
		b->y = ((float)(greysum_y) / greysum_total - 1);
	}
	b->vx = 0;
	b->vy = 0;

	b->left = bounds.left;
	b->top = bounds.top;
	b->width = bounds.right - bounds.left + 1;
	b->height = bounds.bottom - bounds.top + 1;
	b->area = area;
	b->age = 0;
	b->track_index = -1;
	b->id_age = 0;
	b->prev_led_id = b->led_id = LED_INVALID_ID;
	b->greysum = greysum_total;
}

static void
extent_to_blobs(
    struct t_rift_blobwatch *bw, struct blobservation *ob, struct extent *e, uint16_t y, struct xrt_frame *frame)
{
	const uint32_t max_blobs = XRT_CONSTELLATION_MAX_BLOBS_PER_FRAME;

	// Don't store unless there was at least one "bright enough" pixel in the blob
	if (e->max_pixel < bw->params.blob_required_threshold) {
		ob->dropped_dark_blobs++;
		return;
	}

	// Don't store 1x1 blobs
	if (e->top == y && e->left == e->right) {
		return;
	}

	// Check width and height against the blob "maximum size"
	if (y - e->top > bw->params.max_blob_width || e->right - e->left > bw->params.max_blob_width) {
		return;
	}

	// Another extent of the same blob already collected it, the scanline pass splits blobs into several extents
	// when their rows don't overlap enough, such as two touching LEDs
	if (bw->scratch.visited[e->top * frame->width + e->seed_x] == bw->scratch.generation) {
		return;
	}

	uint32_t count;
	uint32_t peak_count;
	struct blob_bounds bounds;
	if (!collect_blob_pixels(bw, frame, e->seed_x, e->top, &count, &peak_count, &bounds)) {
		return;
	}

	// Touching LEDs threshold into one blob, split it at each clear peak, the pixels all start on the first peak
	if (peak_count > 1) {
		peak_count = split_blob_peaks(bw, count, &bounds);
	}
	for (uint32_t peak = 0; peak < peak_count && ob->num_blobs < max_blobs; peak++) {
		store_blob(bw, ob, count, peak);
	}
}

/*
 * Collects contiguous ranges of pixels with values larger than a threshold of
 * THRESHOLD in a given scanline and stores them in extents. Processing stops after
 * num_extents.
 * Extents are marked with the same index as overlapping extents of the previous
 * scanline, and properties of the formed blobs are accumulated.
 *
 * Returns the number of extents found.
 */
static void
process_scanline(uint8_t *line,
                 struct t_rift_blobwatch *bw,
                 uint32_t y,
                 struct extent_line *el,
                 struct extent_line *prev_el,
                 struct xrt_frame *frame,
                 struct blobservation *ob)
{
	struct extent *le_end = prev_el->extents;
	struct extent *le = prev_el->extents;
	struct extent *extent = el->extents;
	uint16_t num_extents = MAX_EXTENTS_PER_LINE;
	float center;
	uint16_t x;
	uint32_t e = 0;

	if (prev_el) {
		le_end += prev_el->num;
	}

	// @todo Support full xrt_frame widths.
	assert(frame->width < UINT16_MAX);

	// Read once, the compiler can't keep them in registers across the extent_to_blobs calls otherwise
	const uint16_t width = (uint16_t)frame->width;
	const uint8_t threshold = bw->params.pixel_threshold;

	// Dark pixels are skipped 8 at a time, adding 127 - threshold to a byte sets its top bit if it's above the
	// threshold. A carry out of a byte only happens when that byte is above the threshold too, so no bright pixel
	// is skipped.
	const bool skip_words = threshold <= 127;
	const uint64_t skip_add = 0x0101010101010101ull * (uint8_t)(127 - threshold);
	const uint64_t top_bits = 0x8080808080808080ull;

	for (x = 0; x < width; x++) {
		uint16_t start, end;
		bool is_new_extent = true;
		uint8_t max_pixel = 0;

		if (skip_words) {
			while (x + 8 <= width) {
				uint64_t pixels;
				memcpy(&pixels, line + x, sizeof(pixels));
				if (((pixels + skip_add) | pixels) & top_bits) {
					break;
				}
				x += 8;
			}
			if (x >= width) {
				break;
			}
		}

		// Loop until pixel value exceeds threshold
		if (line[x] <= threshold) {
			continue;
		}

		start = x++;

		// Loop until pixel value falls below threshold
		while (x < width && line[x] > threshold) {
			if (line[x] > max_pixel) {
				max_pixel = line[x];
			}
			x++;
		}

		end = x - 1;

		center = (start + end) / 2.0;

		extent->start = start;
		extent->end = end;
		extent->area = x - start;
		extent->max_pixel = max_pixel;

		if (prev_el) {
			// Previous extents without significant overlap are the bottom of finished blobs.
			// Store them into an array.
			while (le < le_end && le->end < center) {
				extent_to_blobs(bw, ob, le, y, frame);
				le++;
			}

			// A previous extent with significant overlap is considered to be part of the same blob.
			if (le < le_end && le->start <= center && le->end >= center) {
				extent->top = le->top;
				extent->seed_x = le->seed_x;
				extent->left = MIN(extent->start, le->left);
				extent->right = MAX(extent->end, le->right);
				if (le->max_pixel > extent->max_pixel)
					extent->max_pixel = le->max_pixel;
				extent->area += le->area;
				is_new_extent = false;
				le++;
			}
		}

		// If this extent is not part of a previous blob, increment the blob index.
		if (is_new_extent) {
			extent->top = y;
			extent->seed_x = start;
			extent->left = extent->start;
			extent->right = extent->end;
		}

		if (++e == num_extents) {
			break;
		}
		extent++;
	}

	if (prev_el) {
		// If there are no more extents on this line, all remaining extents in the previous line are finished
		// blobs. Store them.
		while (le < le_end) {
			extent_to_blobs(bw, ob, le, y, frame);
			le++;
		}
	}

	el->num = e;

	if (y == frame->height - 1) {
		// All extents of the last line are finished blobs, too.
		for (extent = el->extents; extent < el->extents + el->num; extent++) {
			extent_to_blobs(bw, ob, extent, y, frame);
		}
	}
}

//! Ensures the scratch space is large enough for this particular frame
static void
ensure_scratch(struct t_rift_blobwatch *bw, struct xrt_frame *frame)
{
	if (bw->scratch.width != frame->width || bw->scratch.height != frame->height) {
		free(bw->scratch.visited);
		bw->scratch.visited = U_TYPED_ARRAY_CALLOC(uint8_t, frame->width * frame->height);
		bw->scratch.generation = 0;
		bw->scratch.width = frame->width;
		bw->scratch.height = frame->height;
	}

	// Wrapping around would make stale marks from 256 frames ago look current
	if (++bw->scratch.generation == 0) {
		memset(bw->scratch.visited, 0, bw->scratch.width * bw->scratch.height);
		bw->scratch.generation = 1;
	}

	/*
	 * If this is too big then this current data structure is quite poor on memory usage,
	 * so find some better way of handling this if we want to have full-screen blobs.
	 */
	assert(bw->params.max_blob_width <= 256);

	// A blob that fits the maximum width fits this many pixels
	bw->scratch.max_blob_width = bw->params.max_blob_width;
	const uint32_t capacity = (bw->scratch.max_blob_width + 1U) * (bw->scratch.max_blob_width + 1U);
	if (bw->scratch.capacity != capacity) {
		U_ARRAY_REALLOC_OR_FREE(bw->scratch.pixels, struct blob_pixel, capacity);
		U_ARRAY_REALLOC_OR_FREE(bw->scratch.pixel_index, uint32_t, capacity);
		U_ARRAY_REALLOC_OR_FREE(bw->scratch.order, uint32_t, capacity);
		bw->scratch.capacity = capacity;
	}
	if (bw->scratch.stack_capacity < capacity) {
		U_ARRAY_REALLOC_OR_FREE(bw->scratch.stack, uint32_t, capacity);
		bw->scratch.stack_capacity = capacity;
	}
}

//! Frees the scratch memory used by this blobwatch
static void
free_scratch(struct t_rift_blobwatch *bw)
{
	free(bw->scratch.visited);
	free(bw->scratch.stack);
	free(bw->scratch.pixels);
	free(bw->scratch.pixel_index);
	free(bw->scratch.order);
}

/*
 * Processes extents from all scanlines in a frame and stores the
 * resulting blobs in ob->blobs.
 */
static void
process_frame(struct t_rift_blobwatch *bw, struct blobservation *ob, struct xrt_frame *frame)
{
	struct extent_line el1;
	struct extent_line el2;

	assert(frame->format == XRT_FORMAT_L8);

	// Ensure the scratch space is good for this frame
	ensure_scratch(bw, frame);

	ob->num_blobs = 0;
	ob->dropped_dark_blobs = 0;
	ob->timestamp_ns = frame->timestamp;

	uint8_t *line = frame->data;
	process_scanline(line, bw, 0, &el1, NULL, frame, ob);
	line += frame->stride;

	for (uint32_t y = 1; y < frame->height; y++) {
		process_scanline(line, bw, y, y & 1 ? &el2 : &el1, y & 1 ? &el1 : &el2, frame, ob);
		line += frame->stride;
	}
}

/*
 * Finds the first free tracking slot.
 */
static int16_t
find_free_track(uint8_t *tracked)
{
	for (uint32_t i = 0; i < XRT_CONSTELLATION_MAX_BLOBS_PER_FRAME; i++) {
		if (tracked[i] == 0) {
			return i;
		}
	}

	return -1;
}

static void
copy_matching_blob(struct blob *to, struct blob *from)
{
	to->blob_id = from->blob_id;
	to->vx = to->x - from->x;
	to->vy = to->y - from->y;
	to->id_age = from->id_age;
	to->led_id = from->led_id;
	to->age = from->age + 1;
}

static void
blobwatch_release_observation(struct t_rift_blobwatch *bw, struct blobservation *ob)
{
	os_mutex_lock(&bw->mutex);
	push_queue(&bw->observation_q, ob);
	os_mutex_unlock(&bw->mutex);
}

/*
 * Detects blobs in the current frame and compares them with the observation
 * history. The returned struct blobservation in the `output` variable must be returned
 * to the blobwatch via blobwatch_release_observation()
 */
static void
blobwatch_process(struct t_rift_blobwatch *bw, struct xrt_frame *frame, struct blobservation **output)
{
	os_mutex_lock(&bw->mutex);
	struct blobservation *ob = pop_queue(&bw->observation_q);

	os_mutex_unlock(&bw->mutex);

	assert(ob != NULL);

	process_frame(bw, ob, frame);

	// Return observed blobs
	if (output) {
		*output = ob;
	}

	// If there is no previous observation, our work is done here - no need to match
	// blobs against the prior
	os_mutex_lock(&bw->mutex);
	if (bw->last_observation == NULL) {
		bw->last_observation = ob;
		os_mutex_unlock(&bw->mutex);
		return;
	}

	struct blobservation *last_ob = bw->last_observation;
	os_mutex_unlock(&bw->mutex);

	int64_t closest_ob[XRT_CONSTELLATION_MAX_BLOBS_PER_FRAME];      // index of last_ob that is closest to each ob
	int64_t closest_last_ob[XRT_CONSTELLATION_MAX_BLOBS_PER_FRAME]; // index of ob that is closest to each last_ob
	int64_t closest_last_ob_distsq[XRT_CONSTELLATION_MAX_BLOBS_PER_FRAME]; // distsq of ob that is closest to each
	                                                                       // last_ob

	// Clear closest_*
	for (uint32_t i = 0; i < XRT_CONSTELLATION_MAX_BLOBS_PER_FRAME; i++) {
		closest_ob[i] = -1;
		closest_last_ob[i] = -1;
		closest_last_ob_distsq[i] = 1000000;
	}

	uint32_t scan_again = 1;
	uint32_t scan_times = 0;
	while (scan_again && scan_times <= 100) {
		scan_again = 0;

		// Try to match each blob with the closest blob from the previous frame.
		for (uint32_t i = 0; i < ob->num_blobs; i++) {
			if (closest_ob[i] != -1) {
				continue; // already has a match
			}

			struct blob *b2 = &ob->blobs[i];
			int64_t closest_distsq = -1;
			int64_t closest_j = -1;

			for (uint32_t j = 0; j < last_ob->num_blobs; j++) {
				struct blob *b1 = &last_ob->blobs[j];
				float x, y, dx, dy, distsq;

				// Estimate b1's next position
				x = b1->x + b1->vx;
				y = b1->y + b1->vy;

				// Absolute distance
				dx = fabsf(x - b2->x);
				dy = fabsf(y - b2->y);
				distsq = dx * dx + dy * dy;

				// Reject matches that are too far away
				if (bw->max_match_dist_sq > 0 && distsq > bw->max_match_dist_sq) {
					continue;
				}

				if (closest_distsq < 0 || distsq < closest_distsq) {
					if (closest_last_ob[j] != -1 && closest_last_ob_distsq[j] <= distsq) {
						// some blob already claimed this one as closest
						// don't usurp if previous one is closer
						continue;
					}
					closest_j = j;
					closest_distsq = distsq;
				}
			}

			closest_ob[i] = closest_j;

			// didn't find any matching blobs
			if (closest_j < 0) {
				continue;
			}

			if (closest_last_ob[closest_j] != -1) {
				// we are usurping some other blob because we are closer
				closest_ob[closest_last_ob[closest_j]] = -1;
				scan_again++;
			}

			closest_last_ob[closest_j] = i;
			closest_last_ob_distsq[closest_j] = closest_distsq;
		}

		scan_times++;
	}

	if (scan_times > 100) {
		U_LOG_W("blob matching looped excessively. scan_times: %d", scan_times);
	}

	// Copy blobs that found a closest match
	for (uint32_t i = 0; i < ob->num_blobs; i++) {
		if (closest_ob[i] < 0) {
			continue; // no match
		}

		struct blob *b2 = &ob->blobs[i];
		struct blob *b1 = &last_ob->blobs[closest_ob[i]];

		if (b1->track_index >= 0 && ob->tracked[b1->track_index] == 0) {
			// Only overwrite tracks that are not already set
			b2->track_index = b1->track_index;
			ob->tracked[b2->track_index] = i + 1;
		}
		copy_matching_blob(b2, b1);
	}

	// Clear the tracking array where blobs have gone missing.
	for (int16_t i = 0; i < XRT_CONSTELLATION_MAX_BLOBS_PER_FRAME; i++) {
		uint8_t t = ob->tracked[i];

		if (t > 0 && ob->blobs[t - 1].track_index != i) {
			ob->tracked[i] = 0;
		}
	}

	// Associate newly tracked blobs with a free space in the
	// tracking array.
	for (uint32_t i = 0; i < ob->num_blobs; i++) {
		struct blob *b2 = &ob->blobs[i];

		if (b2->age > 0 && b2->track_index < 0) {
			b2->track_index = find_free_track(ob->tracked);
		}
		if (b2->track_index >= 0) {
			ob->tracked[b2->track_index] = i + 1;
		}
	}

#if CONSISTENCY_CHECKS
	// Check blob <-> tracked array links for consistency
	for (uint32_t i = 0; i < ob->num_blobs; i++) {
		struct blob *b = &ob->blobs[i];

		if (b->track_index >= 0 && ob->tracked[b->track_index] != i + 1) {
			U_LOG_W("Inconsistency! %d != %d", ob->tracked[b->track_index], i + 1);
		}
	}
#endif

	os_mutex_lock(&bw->mutex);
	bw->last_observation = ob;
	os_mutex_unlock(&bw->mutex);
}

/*
 * xrt_frame_sink implementation
 */

static void
t_rift_blobwatch_push_frame(struct xrt_frame_sink *sink, struct xrt_frame *frame)
{
	struct t_rift_blobwatch *bw = container_of(sink, struct t_rift_blobwatch, frame_sink);

	struct blobservation *output;
	blobwatch_process(bw, frame, &output);

	if (bw->target_sink == NULL) {
		blobwatch_release_observation(bw, output);
		return;
	}

	struct t_blob blobs[XRT_CONSTELLATION_MAX_BLOBS_PER_FRAME];
	for (uint32_t i = 0; i < output->num_blobs; i++) {
		struct blob *b = output->blobs + i;
		struct t_blob *xb = blobs + i;

		// Undistort the blob centre, then map it back through the pinhole intrinsics so that
		// center_undistorted is in *undistorted pixel coordinates* (i.e. what an ideal pinhole
		// camera with the same fx/fy/cx/cy would have measured), not normalized ray tangents.
		float nx, ny;
		t_camera_models_undistort(&bw->camera_model_params, //
		                          b->x,                     //
		                          b->y,                     //
		                          &nx,                      //
		                          &ny);                     //

		xb->blob_id = b->blob_id;
		xb->matched_device_id = LED_OBJECT_ID(b->led_id);
		xb->matched_device_led_id = LED_LOCAL_ID(b->led_id);
		xb->center_distorted.x = b->x;
		xb->center_distorted.y = b->y;
		xb->center_undistorted.x = nx * bw->camera_model_params.fx + bw->camera_model_params.cx;
		xb->center_undistorted.y = ny * bw->camera_model_params.fy + bw->camera_model_params.cy;
		xb->center_homogenized.x = nx;
		xb->center_homogenized.y = ny;
		xb->center_homogenized.z = 1.0f;
		xb->motion_vector.x = b->vx;
		xb->motion_vector.y = b->vy;
		xb->bounding_box.offset.w = b->left;
		xb->bounding_box.offset.h = b->top;
		xb->bounding_box.extent.w = b->width;
		xb->bounding_box.extent.h = b->height;
		xb->size.x = (float)b->width;
		xb->size.y = (float)b->height;
		xb->brightness = (float)((double)b->greysum / b->area / (255. - bw->params.pixel_threshold));
		xb->greysum = (float)((double)b->greysum / (255. - bw->params.pixel_threshold));
	}

	struct t_blob_observation xbo = {
	    .source = &bw->base,
	    .id = (uint64_t)(output - (struct blobservation *)bw->observations),
	    .sequence_id = frame->source_sequence,
	    .timestamp_ns = output->timestamp_ns,
	    .blobs = blobs,
	    .num_blobs = output->num_blobs,
	    .frame = NULL,
	};

	// The sink takes ownership of this reference
	xrt_frame_reference(&xbo.frame, frame);

	t_blob_sink_push_blobs(bw->target_sink, &xbo);
	blobwatch_release_observation(bw, output);
}

/*
 * t_blobwatch implementations
 */

static void
t_rift_blobwatch_node_break_apart(struct xrt_frame_node *node)
{}

static void
t_rift_blobwatch_node_destroy(struct xrt_frame_node *node)
{
	struct t_rift_blobwatch *bw = container_of(node, struct t_rift_blobwatch, node);

	os_mutex_destroy(&bw->mutex);

	u_var_remove_root(bw);

	free_scratch(bw);
	free(bw);
}

static void
t_rift_blobwatch_fake_destroy(struct t_blobwatch *xbw)
{
	// do nothing, the real destroy happens when the frame node is destroyed
}

static void
t_rift_blobwatch_mark_blob_device(struct t_blobwatch *xbw,
                                  const struct t_blob_observation *xbo,
                                  t_constellation_device_id_t device_id)
{
	struct t_rift_blobwatch *bw = t_rift_blobwatch(xbw);

	os_mutex_lock(&bw->mutex);
	// Take the observation in ob and replace any labels for the given device in
	// the most recent observation with labels from this observation
	// FIXME: This is an n^2 match for simplicity. Filtering out blobs with no LED
	// label and sorting the blobs by ID might make things quicker for larger numbers
	// of blobs - needs testing.
	struct blobservation *last_ob = bw->last_observation;
	uint32_t i, l;

	struct blobservation *ob = &bw->observations[(size_t)xbo->id];

	// Copy the updated matching data from the xrt blobservation to the internal blobservation structure
	for (uint32_t i = 0; i < xbo->num_blobs; i++) {
		struct blob *b = ob->blobs + i;
		struct t_blob *xb = xbo->blobs + i;

		if (xb->matched_device_id != device_id) {
			continue; // Not for this device
		}

		b->led_id = LED_MAKE_ID(xb->matched_device_id, xb->matched_device_led_id);
	}

	if (last_ob == NULL || last_ob == ob) {
		// No label transfers needed, increment the ID age
		for (i = 0; i < ob->num_blobs; i++) {
			struct blob *b = ob->blobs + i;
			if (b->led_id != LED_INVALID_ID && b->led_id == b->prev_led_id) {
				b->id_age++;
			} else {
				b->id_age = 0;
			}
		}

		os_mutex_unlock(&bw->mutex);
		return;
	}

	// Clear all labels for the indicated device
	for (l = 0; l < last_ob->num_blobs; l++) {
		struct blob *new_b = last_ob->blobs + l;
		if (LED_OBJECT_ID(new_b->led_id) == device_id) {
			new_b->prev_led_id = new_b->led_id;
			new_b->led_id = LED_INVALID_ID;
		}
	}

	// Transfer all labels for matching blob ids
	for (i = 0; i < ob->num_blobs; i++) {
		struct blob *b = ob->blobs + i;
		if (LED_OBJECT_ID(b->led_id) != device_id) {
			continue; // Not for this device
		}

		for (l = 0; l < last_ob->num_blobs; l++) {
			struct blob *new_b = last_ob->blobs + l;
			if (new_b->blob_id == b->blob_id) {
				U_LOG_D("Found matching blob %u - labelled with LED id %x\n", b->blob_id, b->led_id);

				new_b->led_id = b->led_id;

				if (new_b->led_id == new_b->prev_led_id) {
					new_b->id_age++;
				} else {
					new_b->id_age = 0;
				}
			}
		}
	}

	os_mutex_unlock(&bw->mutex);
}

/*
 * Exported functions
 */

int
t_rift_blobwatch_create(const struct t_rift_blobwatch_params *params,
                        struct xrt_frame_context *xfctx,
                        struct t_blob_sink *blob_sink,
                        struct xrt_frame_sink **out_frame_sink,
                        struct t_blobwatch **out_blobwatch)
{
	struct t_rift_blobwatch *bw = U_TYPED_CALLOC(struct t_rift_blobwatch);

	if (!bw) {
		return -1;
	}

	if (os_mutex_init(&bw->mutex) < 0) {
		free(bw);
		return -1;
	}

	// Convert the camera parameters
	t_camera_model_params_from_t_camera_calibration(&params->camera_calibration, &bw->camera_model_params);

	bw->next_blob_id = 1;

	bw->params = *params;
	bw->max_match_dist_sq = params->max_match_dist * params->max_match_dist;

	bw->last_observation = NULL;

	init_queue(&bw->observation_q);
	// Push all observations into the available queue
	for (uint32_t i = 0; i < NUM_FRAMES_HISTORY; i++) {
		push_queue(&bw->observation_q, bw->observations + i);
	}

	bw->base.mark_blob_device = t_rift_blobwatch_mark_blob_device;
	bw->base.destroy = t_rift_blobwatch_fake_destroy;

	bw->node.break_apart = t_rift_blobwatch_node_break_apart;
	bw->node.destroy = t_rift_blobwatch_node_destroy;

	bw->frame_sink.push_frame = t_rift_blobwatch_push_frame;

	bw->target_sink = blob_sink;

	xrt_frame_context_add(xfctx, &bw->node);

	*out_blobwatch = &bw->base;
	*out_frame_sink = &bw->frame_sink;

	u_var_add_root(bw, "Rift Blobwatch", true);

	u_var_add_gui_header(bw, NULL, "Parameters");
	{
		bw->gui.blob_required_threshold = XRT_C11_COMPOUND(struct u_var_draggable_u8){
		    .val = &bw->params.blob_required_threshold,
		    .min = 0,
		    .max = 255,
		    .step = 1,
		};
		bw->gui.pixel_threshold = XRT_C11_COMPOUND(struct u_var_draggable_u8){
		    .val = &bw->params.pixel_threshold,
		    .min = 0,
		    .max = 255,
		    .step = 1,
		};

		u_var_add_draggable_u8(bw, &bw->gui.pixel_threshold, "pixel_threshold");
		u_var_add_draggable_u8(bw, &bw->gui.blob_required_threshold, "blob_required_threshold");
		u_var_add_f32(bw, &bw->params.max_match_dist, "max_match_dist");
		u_var_add_u16(bw, &bw->params.max_blob_width, "max_blob_width");
	}

	return 0;
}
