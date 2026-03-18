#!/usr/bin/env python3
"""
Magic Wand 訓練腳本 - 與 ESP32-S3 部署相容。
讀取 wanddata.json，rasterize 成 32x32x3，訓練 CNN，匯出 int8 量化 .tflite。

依賴: pip install tensorflow numpy scikit-learn
用法:
  python3 train_model.py path/to/wanddata.json
  輸出: saved_model/, quantized_model.tflite
"""

import glob
import json
import math
import os
import sys
import numpy as np
import tensorflow as tf
from tensorflow import keras
from tensorflow.keras import layers

IMAGE_WIDTH = 32
IMAGE_HEIGHT = 32
NUM_CLASSES = 10
FIXED_POINT = 256

def rasterize_stroke_py(stroke_points, x_range=0.6, y_range=0.6):
    """Rasterize stroke to 32x32x3 uint8 (0-255), same logic as C rasterize_stroke."""
    w, h = IMAGE_WIDTH, IMAGE_HEIGHT
    num_c = 3
    buf = np.full((h * w * num_c), 0, dtype=np.uint8)

    def mul_fp(a, b): return (a * b) // FIXED_POINT
    def div_fp(a, b): return (a * FIXED_POINT) // b if b else 0
    def float_to_fp(a): return int(math.floor(a * FIXED_POINT))
    def norm_to_coord_fp(a_val, range_fp, half_fp):
        a_fp = float_to_fp(a_val)
        norm_fp = div_fp(a_fp, range_fp)
        return mul_fp(norm_fp, half_fp) + half_fp
    def round_fp(a): return (a + FIXED_POINT // 2) // FIXED_POINT
    def gate(a, lo, hi): return max(lo, min(hi, a))

    width_fp = w * FIXED_POINT
    height_fp = h * FIXED_POINT
    half_w_fp = width_fp // 2
    half_h_fp = height_fp // 2
    x_range_fp = float_to_fp(x_range)
    y_range_fp = float_to_fp(y_range)
    n_pts = len(stroke_points)
    if n_pts < 2:
        return buf.reshape(h, w, num_c)
    t_inc_fp = FIXED_POINT // n_pts
    one_half_fp = FIXED_POINT // 2

    for pi in range(n_pts - 1):
        p0 = stroke_points[pi]
        p1 = stroke_points[pi + 1]
        sx_fp = norm_to_coord_fp(p0["x"], x_range_fp, half_w_fp)
        sy_fp = norm_to_coord_fp(-p0["y"], y_range_fp, half_h_fp)
        ex_fp = norm_to_coord_fp(p1["x"], x_range_fp, half_w_fp)
        ey_fp = norm_to_coord_fp(-p1["y"], y_range_fp, half_h_fp)
        dx_fp = ex_fp - sx_fp
        dy_fp = ey_fp - sy_fp

        t_fp = pi * t_inc_fp
        if t_fp < one_half_fp:
            local_t = div_fp(t_fp, one_half_fp)
            omt = FIXED_POINT - local_t
            red = gate(round_fp(omt * 255), 0, 255)
            green = gate(round_fp(local_t * 255), 0, 255)
            blue = 0
        else:
            local_t = div_fp(t_fp - one_half_fp, one_half_fp)
            omt = FIXED_POINT - local_t
            red = 0
            green = gate(round_fp(omt * 255), 0, 255)
            blue = gate(round_fp(local_t * 255), 0, 255)

        if abs(dx_fp) > abs(dy_fp):
            line_len = abs(round_fp(dx_fp))
            if dx_fp > 0:
                x_inc_fp = FIXED_POINT
                y_inc_fp = div_fp(dy_fp, dx_fp)
            else:
                x_inc_fp = -FIXED_POINT
                y_inc_fp = -div_fp(dy_fp, dx_fp)
        else:
            line_len = abs(round_fp(dy_fp))
            if dy_fp > 0:
                y_inc_fp = FIXED_POINT
                x_inc_fp = div_fp(dx_fp, dy_fp)
            else:
                y_inc_fp = -FIXED_POINT
                x_inc_fp = -div_fp(dx_fp, dy_fp)

        for i in range(line_len + 1):
            x_fp = sx_fp + i * x_inc_fp
            y_fp = sy_fp + i * y_inc_fp
            x = round_fp(x_fp)
            y = round_fp(y_fp)
            if 0 <= x < w and 0 <= y < h:
                idx = (y * w * num_c) + x * num_c
                buf[idx + 0] = red
                buf[idx + 1] = green
                buf[idx + 2] = blue

    return buf.reshape(h, w, num_c)

def load_strokes_from_json(path):
    with open(path, "r", encoding="utf-8") as f:
        data = json.load(f)
    strokes = []
    for s in data.get("strokes", []):
        label = s.get("label", "?")
        pts = s.get("strokePoints", s.get("stroke_points", []))
        if not pts:
            continue
        strokes.append({"label": label, "strokePoints": pts})
    return strokes

def main():
    if len(sys.argv) < 2:
        print("Usage: train_model.py <wanddata.json> [more.json ...]", file=sys.stderr)
        sys.exit(1)

    all_strokes = []
    for path in sys.argv[1:]:
        for p in glob.glob(path):
            all_strokes.extend(load_strokes_from_json(p))

    if not all_strokes:
        print("No strokes found.", file=sys.stderr)
        sys.exit(1)

    labels_sorted = sorted(set(s["label"] for s in all_strokes))
    label_to_idx = {l: i for i, l in enumerate(labels_sorted)}
    num_classes = len(labels_sorted)
    if num_classes != NUM_CLASSES:
        print("Warning: found %d classes; adjust NUM_CLASSES if needed." % num_classes)

    X = []
    y = []
    for s in all_strokes:
        img = rasterize_stroke_py(s["strokePoints"])
        X.append(img)
        y.append(label_to_idx[s["label"]])

    X = np.array(X, dtype=np.float32) / 255.0
    y = np.array(y, dtype=np.int32)
    # Convert to int8 range -128..127 for TFLite compatibility
    X_int8 = (X * 255 - 128).astype(np.int8)
    X_for_model = (X_int8.astype(np.float32) + 128) / 255.0  # train in 0-1 range

    from sklearn.model_selection import train_test_split
    X_train, X_val, y_train, y_val = train_test_split(X_for_model, y, test_size=0.2, random_state=42)

    def make_model():
        inp = keras.Input(shape=(IMAGE_HEIGHT, IMAGE_WIDTH, 3))
        x = layers.Conv2D(16, 3, strides=2, padding="same")(inp)
        x = layers.ReLU()(x)
        x = layers.Conv2D(32, 3, strides=2, padding="same")(x)
        x = layers.ReLU()(x)
        x = layers.Conv2D(64, 3, strides=2, padding="same")(x)
        x = layers.ReLU()(x)
        x = layers.GlobalAveragePooling2D()(x)
        x = layers.Dense(num_classes)(x)
        out = layers.Softmax()(x)
        return keras.Model(inp, out)

    model = make_model()
    model.compile(optimizer="adam", loss="sparse_categorical_crossentropy", metrics=["accuracy"])
    model.fit(X_train, y_train, validation_data=(X_val, y_val), epochs=20, batch_size=32)

    os.makedirs("saved_model", exist_ok=True)
    model.save("saved_model")

    # Export float TFLite
    converter = tf.lite.TFLiteConverter.from_saved_model("saved_model")
    tflite_float = converter.convert()
    with open("float_model.tflite", "wb") as f:
        f.write(tflite_float)

    # Full integer quantization (int8)
    def rep_data():
        for i in range(min(100, len(X_int8))):
            yield [X_int8[i:i+1].astype(np.float32)]

    converter = tf.lite.TFLiteConverter.from_saved_model("saved_model")
    converter.optimizations = [tf.lite.Optimize.DEFAULT]
    converter.target_spec.supported_ops = [tf.lite.OpsSet.TFLITE_BUILTINS_INT8]
    converter.inference_input_type = tf.int8
    converter.inference_output_type = tf.int8
    converter.representative_dataset = rep_data
    tflite_quant = converter.convert()
    with open("quantized_model.tflite", "wb") as f:
        f.write(tflite_quant)

    print("Done. Use: python3 tflite_to_c_array.py quantized_model.tflite ../esp32s3/main/model_data.c")
    print("Labels in order:", labels_sorted)

if __name__ == "__main__":
    main()
