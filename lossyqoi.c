#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

// QOI constants
#define QOI_OP_INDEX  0x00
#define QOI_OP_DIFF   0x40
#define QOI_OP_LUMA   0x80
#define QOI_OP_RUN    0xc0
#define QOI_OP_RGB    0xfe
#define QOI_OP_RGBA   0xff

typedef struct {
    uint8_t r, g, b, a;
} qoi_rgba;

typedef struct {
    FILE *file;
    int run;
    qoi_rgba prev_px, index[64];
    int lossiness; // Only for runs
} qoi_encoder;

// Write bytes to file (endian-safe)
static void qoi_write_bytes(FILE *f, const void *data, int size) {
    fwrite(data, 1, size, f);
}

// Initialize encoder
static void qoi_encoder_init(qoi_encoder *enc, FILE *file, int lossiness) {
    memset(enc, 0, sizeof(*enc));
    enc->file = file;
    enc->prev_px.a = 255; // Default alpha
    enc->lossiness = lossiness;
}

// Hash function for color index
static int qoi_color_hash(qoi_rgba px) {
    return (px.r * 3 + px.g * 5 + px.b * 7 + px.a * 11) % 64;
}

// Check if two pixels are "close enough" (for lossy runs)
static int qoi_px_near(qoi_rgba a, qoi_rgba b, int threshold) {
    return (abs(a.r - b.r) <= threshold &&
            abs(a.g - b.g) <= threshold &&
            abs(a.b - b.b) <= threshold &&
            a.a == b.a);
}

// Write a single pixel (with standard DIFF/LUMA and lossy runs)
static void qoi_write_pixel(qoi_encoder *enc, qoi_rgba px) {
    // --- Lossy Run Handling ---
    if (qoi_px_near(px, enc->prev_px, enc->lossiness)) {
        enc->run++;
        //if (enc->run == 1) enc->run = 2; // Start run
        if (enc->run == 62) {
            uint8_t op = QOI_OP_RUN | 61;
            qoi_write_bytes(enc->file, &op, 1);
            enc->run = 0;
        }
        return;
    }

    if (enc->run > 0) {
        uint8_t op = QOI_OP_RUN | (enc->run - 1);
        qoi_write_bytes(enc->file, &op, 1);
        enc->run = 0;
    }

    // --- Standard QOI Encoding ---
    int index_pos = qoi_color_hash(px);
    if (memcmp(&enc->index[index_pos], &px, sizeof(px)) == 0) {
        uint8_t op = QOI_OP_INDEX | index_pos;
        qoi_write_bytes(enc->file, &op, 1);
    } else {
        enc->index[index_pos] = px;

        int dr = px.r - enc->prev_px.r;
        int dg = px.g - enc->prev_px.g;
        int db = px.b - enc->prev_px.b;
        int da = px.a - enc->prev_px.a;

        // Try DIFF (-2..1 per channel)
        if (da == 0 && dr >= -2 && dr <= 1 && 
            dg >= -2 && dg <= 1 && db >= -2 && db <= 1) {
            uint8_t op = QOI_OP_DIFF | ((dr + 2) << 4) | ((dg + 2) << 2) | (db + 2);
            qoi_write_bytes(enc->file, &op, 1);
        }
        // Try LUMA (-32..31 for dg, -8..7 for dr-dg/db-dg)
        else if (da == 0 && dg >= -32 && dg <= 31 &&
                 (dr - dg) >= -8 && (dr - dg) <= 7 &&
                 (db - dg) >= -8 && (db - dg) <= 7) {
            uint8_t op1 = QOI_OP_LUMA | (dg + 32);
            uint8_t op2 = ((dr - dg + 8) << 4) | (db - dg + 8);
            qoi_write_bytes(enc->file, &op1, 1);
            qoi_write_bytes(enc->file, &op2, 1);
        }
        // Fall back to RGB/RGBA
        else if (px.a == enc->prev_px.a) {
            uint8_t op = QOI_OP_RGB;
            qoi_write_bytes(enc->file, &op, 1);
            qoi_write_bytes(enc->file, &px.r, 3);
        } else {
            uint8_t op = QOI_OP_RGBA;
            qoi_write_bytes(enc->file, &op, 1);
            qoi_write_bytes(enc->file, &px, 4);
        }
    }
    enc->prev_px = px;
}

// Write QOI header
static void qoi_write_header(qoi_encoder *enc, int width, int height, int channels) {
    const uint8_t header[] = {
        'q', 'o', 'i', 'f',
        (uint8_t)(width >> 24), (uint8_t)(width >> 16),
        (uint8_t)(width >> 8), (uint8_t)width,
        (uint8_t)(height >> 24), (uint8_t)(height >> 16),
        (uint8_t)(height >> 8), (uint8_t)height,
        (uint8_t)channels, 0
    };
    qoi_write_bytes(enc->file, header, sizeof(header));
}

// Write QOI footer
static void qoi_write_footer(qoi_encoder *enc) {
    const uint8_t footer[] = {0, 0, 0, 0, 0, 0, 0, 1};
    qoi_write_bytes(enc->file, footer, sizeof(footer));
}

void convert_to_qoi(const char *input_path, const char *output_path, int lossiness) {
    int width, height, channels;
    uint8_t *data = stbi_load(input_path, &width, &height, &channels, 0);
    if (!data) {
        fprintf(stderr, "Error loading image: %s\n", input_path);
        return;
    }

    FILE *out = fopen(output_path, "wb");
    if (!out) {
        stbi_image_free(data);
        fprintf(stderr, "Error opening output file: %s\n", output_path);
        return;
    }

    qoi_encoder enc;
    qoi_encoder_init(&enc, out, lossiness);
    qoi_write_header(&enc, width, height, channels);

    for (int i = 0; i < width * height; i++) {
        qoi_rgba px;
        px.r = data[i * channels];
        px.g = data[i * channels + (channels > 1 ? 1 : 0)];
        px.b = data[i * channels + (channels > 2 ? 2 : 0)];
        px.a = (channels == 4) ? data[i * channels + 3] : 255;
        qoi_write_pixel(&enc, px);
    }

    // Flush final run if needed
    if (enc.run > 0) {
        uint8_t op = QOI_OP_RUN | (enc.run - 1);
        qoi_write_bytes(out, &op, 1);
    }

    qoi_write_footer(&enc);
    fclose(out);
    stbi_image_free(data);
}

int main(int argc, char **argv) {
    if (argc < 3) {
        printf("Usage: %s <input.png/jpg> <output.qoi> [lossiness=0]\n", argv[0]);
        return 1;
    }
    int lossiness = (argc > 3) ? atoi(argv[3]) : 0;
    convert_to_qoi(argv[1], argv[2], lossiness);
    return 0;
}
