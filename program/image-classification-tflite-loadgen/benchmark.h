/*
 * Copyright (c) 2018 cTuning foundation.
 * See CK COPYRIGHT.txt for copyright details.
 *
 * SPDX-License-Identifier: BSD-3-Clause.
 * See CK LICENSE.txt for licensing details.
 */
 
#pragma once

#include <stdio.h>
#include <stdlib.h>

#include <dirent.h>
#include <fstream>
#include <iostream>
#include <memory>
#include <string.h>
#include <thread>
#include <vector>

#include <xopenme.h>

#define DEBUG(msg) std::cout << "DEBUG: " << msg << std::endl;

namespace CK {

enum MODEL_TYPE {
  LITE,
  TF_FROZEN
};

/// Store named value into xopenme variable.
inline void store_value_f(int index, const char* name, float value) {
  char* json_name = new char[strlen(name) + 6];
  sprintf(json_name, "\"%s\":%%f", name);
  xopenme_add_var_f(index, json_name, value);
  delete[] json_name;
}

/// Load mandatory string value from the environment.
inline std::string getenv_s(const std::string& name) {
  const char *value = getenv(name.c_str());
  if (!value)
    throw "Required environment variable " + name + " is not set";
  return std::string(value);
}

/// Load mandatory integer value from the environment.
inline int getenv_i(const std::string& name) {
  const char *value = getenv(name.c_str());
  if (!value)
    throw "Required environment variable " + name + " is not set";
  return atoi(value);
}

/// Load mandatory float value from the environment.
inline float getenv_f(const std::string& name) {
  const char *value = getenv(name.c_str());
  if (!value)
    throw "Required environment variable " + name + " is not set";
  return atof(value);
}

/// Dummy `sprintf` like formatting function using std::string.
/// It uses buffer of fixed length so can't be used in any cases,
/// generally use it for short messages with numeric arguments.
template <typename ...Args>
inline std::string format(const char* str, Args ...args) {
  char buf[1024];
  sprintf(buf, str, args...);
  return std::string(buf);
}

//----------------------------------------------------------------------

class Accumulator {
public:
  void reset() { _total = 0, _count = 0; }
  void add(float value) { _total += value, _count++; }
  float total() const { return _total; }
  float avg() const { return _total / static_cast<float>(_count); }
private:
  float _total = 0;
  int _count = 0;
};

//----------------------------------------------------------------------

class BenchmarkSettings {
public:
  const std::string images_dir = getenv_s("CK_ENV_DATASET_IMAGENET_PREPROCESSED_DIR");
  const std::string images_file = getenv_s("CK_ENV_DATASET_IMAGENET_PREPROCESSED_SUBSET_FOF");
  const bool skip_internal_preprocessing = getenv("CK_ENV_DATASET_IMAGENET_PREPROCESSED_DATA_TYPE")
                        && ( getenv_s("CK_ENV_DATASET_IMAGENET_PREPROCESSED_DATA_TYPE") == "float32" );

  const std::string result_dir = getenv_s("CK_RESULTS_DIR");
  const std::string input_layer_name = getenv_s("CK_ENV_TENSORFLOW_MODEL_INPUT_LAYER_NAME");
  const std::string output_layer_name = getenv_s("CK_ENV_TENSORFLOW_MODEL_OUTPUT_LAYER_NAME");
  const int batch_count = getenv_i("CK_BATCH_COUNT");
  const int batch_size = getenv_i("CK_BATCH_SIZE");
  const int image_size = getenv_i("CK_ENV_DATASET_IMAGENET_PREPROCESSED_INPUT_SQUARE_SIDE");
  const int num_channels = 3;
  const int num_classes = 1000;
  const bool normalize_img = getenv_s("CK_ENV_TENSORFLOW_MODEL_NORMALIZE_DATA") == "YES";
  const bool subtract_mean = getenv_s("CK_ENV_TENSORFLOW_MODEL_SUBTRACT_MEAN") == "YES";
  const bool full_report = getenv_i("CK_SILENT_MODE") == 0;

  BenchmarkSettings(enum MODEL_TYPE mode = MODEL_TYPE::LITE) {

    switch (mode)
    {
    case MODEL_TYPE::LITE:
      _graph_file = getenv_s("CK_ENV_TENSORFLOW_MODEL_TFLITE_FILEPATH");
      break;
    
    case MODEL_TYPE::TF_FROZEN:
      _graph_file = getenv_s("CK_ENV_TENSORFLOW_MODEL_TF_FROZEN_FILEPATH");
      break;
    
    default:
      std::cout << "Unsupported MODEL_TYPE" << std::endl;
      exit(-1);
      break;
    };
    _number_of_threads = std::thread::hardware_concurrency();
    _number_of_threads = _number_of_threads < 1 ? 1 : _number_of_threads;
    _number_of_threads = !getenv("CK_HOST_CPU_NUMBER_OF_PROCESSORS")
                         ? _number_of_threads
                         : getenv_i("CK_HOST_CPU_NUMBER_OF_PROCESSORS");

    // Print settings
    std::cout << "Graph file: " << _graph_file << std::endl;
    std::cout << "Image dir: " << images_dir << std::endl;
    std::cout << "Image list: " << images_file << std::endl;
    std::cout << "Image size: " << image_size << std::endl;
    std::cout << "Image channels: " << num_channels << std::endl;
    std::cout << "Prediction classes: " << num_classes << std::endl;
    std::cout << "Result dir: " << result_dir << std::endl;
    std::cout << "Batch count: " << batch_count << std::endl;
    std::cout << "Batch size: " << batch_size << std::endl;
    std::cout << "Normalize: " << normalize_img << std::endl;
    std::cout << "Subtract mean: " << subtract_mean << std::endl;

    // Create results dir if none
    auto dir = opendir(result_dir.c_str());
    if (dir)
      closedir(dir);
    else
      system(("mkdir " + result_dir).c_str());

    // Load list of images to be processed
    std::ifstream file(images_file);
    if (!file)
      throw "Unable to open image list file " + images_file;
    for (std::string s; !getline(file, s).fail();)
      _image_list.emplace_back(s);
    std::cout << "Image count in file: " << _image_list.size() << std::endl;
  }

  const std::vector<std::string>& image_list() const { return _image_list; }

  std::vector<std::string> _image_list;

  int number_of_threads() { return _number_of_threads; }

  std::string graph_file() { return _graph_file; }
private:
  int _number_of_threads;
  std::string _graph_file;
};

//----------------------------------------------------------------------

class BenchmarkSession {
public:
  BenchmarkSession(const BenchmarkSettings* settings): _settings(settings) {
  }

  virtual ~BenchmarkSession() {}

  bool get_next_batch() {
    if (_batch_index+1 == _settings->batch_count)
      return false;
    _batch_index++;
    int batch_number = _batch_index+1;
    if (_settings->full_report || batch_number%10 == 0)
      std::cout << "\nBatch " << batch_number << " of " << _settings->batch_count << std::endl;
    int begin = _batch_index * _settings->batch_size;
    int end = (_batch_index + 1) * _settings->batch_size;
    int images_count = _settings->image_list().size();
    if (begin >= images_count || end > images_count)
      throw format("Not enough images to populate batch %d", _batch_index);
    _batch_files.clear();
    for (int i = begin; i < end; i++)
      _batch_files.emplace_back(_settings->image_list()[i]);
    return true;
  }

  int batch_index() const { return _batch_index; }
  const std::vector<std::string>& batch_files() const { return _batch_files; }

private:
  int _batch_index = -1;

  const BenchmarkSettings* _settings;
  std::vector<std::string> _batch_files;
};

//----------------------------------------------------------------------

template <typename TData>
class StaticBuffer {
public:
  StaticBuffer(int size, const std::string& dir): _size(size), _dir(dir) {
    _buffer = new TData[size];
  }
  
  virtual ~StaticBuffer() {
    delete[] _buffer;
  }

  TData* data() const { return _buffer; }
  int size() const { return _size; }

protected:
  const int _size;
  const std::string _dir;
  TData* _buffer;
};

//----------------------------------------------------------------------

class ImageData : public StaticBuffer<uint8_t> {
public:
  ImageData(const BenchmarkSettings* s): StaticBuffer(
    s->image_size * s->image_size * s->num_channels * (s->skip_internal_preprocessing ? sizeof(float) : sizeof(uint8_t)),
    s->images_dir) {}
  
  void load(const std::string& filename) {
    auto path = _dir + '/' + filename;
    std::ifstream file(path, std::ios::in | std::ios::binary);
    if (!file) throw "Failed to open image data " + path;
    file.read(reinterpret_cast<char*>(_buffer), _size);
  }
};

//----------------------------------------------------------------------

class ResultData : public StaticBuffer<float> {
public:
  ResultData(const BenchmarkSettings* s): StaticBuffer<float>(
    s->num_classes, s->result_dir) {}

  void save(const std::string& filename) {
    auto path = _dir + '/' + filename + ".txt";
    std::ofstream file(path);
    if (!file) throw "Unable to create result file " + path;
    for (int i = 0; i < _size; i++)
      file << _buffer[i] << std::endl;
  }
};

//----------------------------------------------------------------------

class IBenchmark {
public:
  bool has_background_class = false;

  virtual ~IBenchmark() {}
  virtual void load_images(const std::vector<std::string>& batch_images) = 0;
  virtual void save_results(const std::vector<std::string>& batch_images) = 0;
};


template <typename TData, typename TInConverter, typename TOutConverter>
class Benchmark : public IBenchmark {
public:
  Benchmark(const BenchmarkSettings* settings, TData *in_ptr, TData *out_ptr) {
    _in_ptr = in_ptr;
    _out_ptr = out_ptr;
    _in_data.reset(new ImageData(settings));
    _out_data.reset(new ResultData(settings));
    _in_converter.reset(new TInConverter(settings));
    _out_converter.reset(new TOutConverter(settings));
  }

  void load_images(const std::vector<std::string>& batch_images) override {
    int image_offset = 0;
    for (auto image_file : batch_images) {
      _in_data->load(image_file);
      _in_converter->convert(_in_data.get(), _in_ptr + image_offset);
      image_offset += _in_data->size();
    }
  }

  void save_results(const std::vector<std::string>& batch_images) override {
    int image_offset = 0;
    int probe_offset = has_background_class ? 1 : 0;
    for (auto image_file : batch_images) {
      _out_converter->convert(_out_ptr + image_offset + probe_offset, _out_data.get());
      _out_data->save(image_file);
      image_offset += _out_data->size() + probe_offset;
    }
  }

private:
  TData* _in_ptr;
  TData* _out_ptr;
  std::unique_ptr<ImageData> _in_data;
  std::unique_ptr<ResultData> _out_data;
  std::unique_ptr<TInConverter> _in_converter;
  std::unique_ptr<TOutConverter> _out_converter;
};

//----------------------------------------------------------------------

class IinputConverter {
public:
  virtual ~IinputConverter() {}
  virtual void convert(const ImageData* source, void* target) = 0;
};

//----------------------------------------------------------------------

class InCopy : public IinputConverter {
public:
  InCopy(const BenchmarkSettings* s) {}
  
  void convert(const ImageData* source, void* target) {
    uint8_t *uint8_target = static_cast<uint8_t *>(target);
    std::copy(source->data(), source->data() + source->size(), uint8_target);
  }
};

//----------------------------------------------------------------------

class InNormalize : public IinputConverter {
public:
  InNormalize(const BenchmarkSettings* s):
    _normalize_img(s->normalize_img), _subtract_mean(s->subtract_mean) {
  }
  
  void convert(const ImageData* source, void* target) {
    // Copy image data to target
    float *float_target = static_cast<float *>(target);
    float sum = 0;
    for (int i = 0; i < source->size(); i++) {
      float px = source->data()[i];
      if (_normalize_img)
        px = (px / 255.0 - 0.5) * 2.0;
      sum += px;
      float_target[i] = px;
    }
    // Subtract mean value if required
    if (_subtract_mean) {
      float mean = sum / static_cast<float>(source->size());
      for (int i = 0; i < source->size(); i++)
        float_target[i] -= mean;
    }
  }

private:
  const bool _normalize_img;
  const bool _subtract_mean;
};

//----------------------------------------------------------------------

class OutCopy {
public:
  OutCopy(const BenchmarkSettings* s) {}
  
  void convert(const float* source, ResultData* target) const {
    std::copy(source, source + target->size(), target->data());
  }
};

//----------------------------------------------------------------------

class OutDequantize {
public:
  OutDequantize(const BenchmarkSettings* s) {}
  
  void convert(const uint8_t* source, ResultData* target) const {
    for (int i = 0; i < target->size(); i++)
      target->data()[i] = source[i] / 255.0;
  }
};

} // namespace CK