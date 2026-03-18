/* Model data - replace model_data.c with output from train_esp32s3 after training.
 * See: train_esp32s3/README.md and run tflite_to_c_array.py on your .tflite file.
 */

#ifndef MODEL_DATA_H
#define MODEL_DATA_H

#ifdef __cplusplus
extern "C" {
#endif

extern const unsigned char g_magic_wand_model_data[];
extern const unsigned int g_magic_wand_model_data_len;

#ifdef __cplusplus
}
#endif

#endif /* MODEL_DATA_H */
