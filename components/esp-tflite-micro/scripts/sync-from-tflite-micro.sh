#!/usr/bin/env bash

set -e -x

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="${SCRIPT_DIR}/.."
SRC_DIR="${ROOT_DIR}/src"
mkdir -p ${SRC_DIR}
cd "${SRC_DIR}"

TEMP_DIR=$(mktemp -d)
cd "${TEMP_DIR}"

# Clone the TFLM
echo Cloning tflite-micro repo to "${TEMP_DIR}"
git clone --depth 1 --single-branch "https://github.com/tensorflow/tflite-micro.git"
cd tflite-micro

# Create the TFLM base tree
python3 tensorflow/lite/micro/tools/project_generation/create_tflm_tree.py \
  -e hello_world \
  -e micro_speech \
  -e person_detection \
  "${TEMP_DIR}/tflm-out"

# Backup `micro/kernels/esp_nn` directory to new tree
cp -r "${SRC_DIR}"/tensorflow/lite/micro/kernels/esp_nn \
 "${TEMP_DIR}"/tflm-out/tensorflow/lite/micro/kernels/

cd "${SRC_DIR}"
rm -rf tensorflow
rm -rf third_party
rm -rf signal
mv "${TEMP_DIR}/tflm-out/tensorflow" tensorflow

# For this repo we are forking both the models and the examples.
rm -rf tensorflow/lite/micro/models
mkdir -p third_party/
cp -r "${TEMP_DIR}"/tflm-out/third_party/* third_party/
mkdir -p signal/
cp -r "${TEMP_DIR}"/tflm-out/signal/* signal/

rm -rf "${TEMP_DIR}"
