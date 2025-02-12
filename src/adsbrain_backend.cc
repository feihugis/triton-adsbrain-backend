// Copyright 2021-2022, MICROSOFT CORPORATION & AFFILIATES. All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions
// are met:
//  * Redistributions of source code must retain the above copyright
//    notice, this list of conditions and the following disclaimer.
//  * Redistributions in binary form must reproduce the above copyright
//    notice, this list of conditions and the following disclaimer in the
//    documentation and/or other materials provided with the distribution.
//  * Neither the name of MICROSOFT CORPORATION nor the names of its
//    contributors may be used to endorse or promote products derived
//    from this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS ``AS IS'' AND ANY
// EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
// PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR
// CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
// EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
// PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
// PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
// OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#include "adsbrain_backend.h"

#include <dlfcn.h>

#include "triton/backend/backend_common.h"
#include "triton/backend/backend_input_collector.h"
#include "triton/backend/backend_model.h"
#include "triton/backend/backend_model_instance.h"
#include "triton/backend/backend_output_responder.h"
#include "triton/core/tritonbackend.h"

namespace triton { namespace backend { namespace adsbrain {

//
// Adsbrain Backend for C++ based model. This backend works
// for any model that has 1 input with STRING datatype and the shape [1] and
// 1 output with the shape [1] and STRING datatype. The backend
// supports both batching and non-batching models.
//

/////////////

extern "C" {

// Triton calls TRITONBACKEND_Initialize when a backend is loaded into
// Triton to allow the backend to create and initialize any state that
// is intended to be shared across all models and model instances that
// use the backend. The backend should also verify version
// compatibility with Triton in this function.
//
TRITONSERVER_Error*
TRITONBACKEND_Initialize(TRITONBACKEND_Backend* backend)
{
  const char* cname;
  RETURN_IF_ERROR(TRITONBACKEND_BackendName(backend, &cname));
  std::string name(cname);

  LOG_MESSAGE(
      TRITONSERVER_LOG_INFO,
      (std::string("TRITONBACKEND_Initialize: ") + name).c_str());

  // Check the backend API version that Triton supports vs. what this
  // backend was compiled against. Make sure that the Triton major
  // version is the same and the minor version is >= what this backend
  // uses.
  uint32_t api_version_major, api_version_minor;
  RETURN_IF_ERROR(
      TRITONBACKEND_ApiVersion(&api_version_major, &api_version_minor));

  LOG_MESSAGE(
      TRITONSERVER_LOG_INFO,
      (std::string("Triton TRITONBACKEND API version: ") +
       std::to_string(api_version_major) + "." +
       std::to_string(api_version_minor))
          .c_str());
  LOG_MESSAGE(
      TRITONSERVER_LOG_INFO,
      (std::string("'") + name + "' TRITONBACKEND API version: " +
       std::to_string(TRITONBACKEND_API_VERSION_MAJOR) + "." +
       std::to_string(TRITONBACKEND_API_VERSION_MINOR))
          .c_str());

  if ((api_version_major != TRITONBACKEND_API_VERSION_MAJOR) ||
      (api_version_minor < TRITONBACKEND_API_VERSION_MINOR)) {
    return TRITONSERVER_ErrorNew(
        TRITONSERVER_ERROR_UNSUPPORTED,
        "triton backend API version does not support this backend");
  }

  // The backend configuration may contain information needed by the
  // backend, such as tritonserver command-line arguments. This
  // backend doesn't use any such configuration but for this example
  // print whatever is available.
  TRITONSERVER_Message* backend_config_message;
  RETURN_IF_ERROR(
      TRITONBACKEND_BackendConfig(backend, &backend_config_message));

  const char* buffer;
  size_t byte_size;
  RETURN_IF_ERROR(TRITONSERVER_MessageSerializeToJson(
      backend_config_message, &buffer, &byte_size));
  LOG_MESSAGE(
      TRITONSERVER_LOG_INFO,
      (std::string("backend configuration:\n") + buffer).c_str());

  // This backend does not require any "global" state but as an
  // example create a string to demonstrate.
  std::string* state = new std::string("backend state");
  RETURN_IF_ERROR(
      TRITONBACKEND_BackendSetState(backend, reinterpret_cast<void*>(state)));

  return nullptr;  // success
}

// Triton calls TRITONBACKEND_Finalize when a backend is no longer
// needed.
//
TRITONSERVER_Error*
TRITONBACKEND_Finalize(TRITONBACKEND_Backend* backend)
{
  // Delete the "global" state associated with the backend.
  void* vstate;
  RETURN_IF_ERROR(TRITONBACKEND_BackendState(backend, &vstate));
  std::string* state = reinterpret_cast<std::string*>(vstate);

  LOG_MESSAGE(
      TRITONSERVER_LOG_INFO,
      (std::string("TRITONBACKEND_Finalize: state is '") + *state + "'")
          .c_str());

  delete state;

  return nullptr;  // success
}

}  // extern "C"

/////////////

//
// ModelState
//
// State associated with a model that is using this backend. An object
// of this class is created and associated with each
// TRITONBACKEND_Model. ModelState is derived from BackendModel class
// provided in the backend utilities that provides many common
// functions.
//
class ModelState : public BackendModel {
 public:
  static TRITONSERVER_Error* Create(
      TRITONBACKEND_Model* triton_model, ModelState** state);
  virtual ~ModelState() = default;

  // Name of the input and output tensor
  const std::string& InputTensorName() const { return input_name_; }
  const std::string& OutputTensorName() const { return output_name_; }
  const std::unordered_map<std::string, std::string>& GetModelConfig() const
  {
    return adsbrain_model_configurations_;
  }

  // Datatype of the input and output tensor
  TRITONSERVER_DataType TensorDataType() const { return datatype_; }

  // Shape of the input and output tensor as given in the model
  // configuration file. This shape will not include the batch
  // dimension (if the model has one).
  const std::vector<int64_t>& TensorNonBatchShape() const { return nb_shape_; }

  // Shape of the input and output tensor, including the batch
  // dimension (if the model has one). This method cannot be called
  // until the model is completely loaded and initialized, including
  // all instances of the model. In practice, this means that backend
  // should only call it in TRITONBACKEND_ModelInstanceExecute.
  TRITONSERVER_Error* TensorShape(std::vector<int64_t>& shape);

  // Validate that this model is supported by this backend.
  TRITONSERVER_Error* ValidateModelConfig();

 private:
  ModelState(TRITONBACKEND_Model* triton_model);

  std::string input_name_;
  std::string output_name_;

  TRITONSERVER_DataType datatype_;

  bool shape_initialized_;
  std::vector<int64_t> nb_shape_;
  std::vector<int64_t> shape_;
  std::unordered_map<std::string, std::string> adsbrain_model_configurations_;
};

ModelState::ModelState(TRITONBACKEND_Model* triton_model)
    : BackendModel(triton_model), shape_initialized_(false)
{
  // Validate that the model's configuration matches what is supported
  // by this backend.
  THROW_IF_BACKEND_MODEL_ERROR(ValidateModelConfig());

  // Load the model configuration parameters
  std::string RELATIVE_PATH_KEYWORD = "$$TRITON_MODEL_DIRECTORY";
  const char* cur_model_dir = nullptr;
  TRITONBACKEND_ArtifactType artifact_type;
  THROW_IF_BACKEND_MODEL_ERROR(TRITONBACKEND_ModelRepository(
      TritonModel(), &artifact_type, &cur_model_dir));

  triton::common::TritonJson::Value params;
  if (ModelConfig().Find("parameters", &params)) {
    std::vector<std::string> names;
    THROW_IF_BACKEND_MODEL_ERROR(params.Members(&names));
    for (size_t i = 0; i < names.size(); ++i) {
      std::string key = names[i];
      std::string value;
      THROW_IF_BACKEND_MODEL_ERROR(
          TryParseModelStringParameter(params, key.c_str(), &value, "NULL"));
      size_t relative_path_loc = value.find(RELATIVE_PATH_KEYWORD);
      if (relative_path_loc != std::string::npos) {
        value.replace(
            relative_path_loc, RELATIVE_PATH_KEYWORD.length(), cur_model_dir);
      }

      adsbrain_model_configurations_.emplace(std::make_pair(key, value));
    }
  }

  if (adsbrain_model_configurations_.find("model_lib_path") ==
      adsbrain_model_configurations_.end()) {
    LOG_MESSAGE(
        TRITONSERVER_LOG_ERROR,
        "failed to find 'model_lib_path' in model config file");
  }
}

TRITONSERVER_Error*
ModelState::Create(TRITONBACKEND_Model* triton_model, ModelState** state)
{
  try {
    *state = new ModelState(triton_model);
  }
  catch (const BackendModelException& ex) {
    RETURN_ERROR_IF_TRUE(
        ex.err_ == nullptr, TRITONSERVER_ERROR_INTERNAL,
        std::string("unexpected nullptr in BackendModelException"));
    RETURN_IF_ERROR(ex.err_);
  }

  return nullptr;  // success
}

TRITONSERVER_Error*
ModelState::TensorShape(std::vector<int64_t>& shape)
{
  // This backend supports models that batch along the first dimension
  // and those that don't batch. For non-batch models the output shape
  // will be the shape from the model configuration. For batch models
  // the output shape will be the shape from the model configuration
  // prepended with [ -1 ] to represent the batch dimension. The
  // backend "responder" utility used below will set the appropriate
  // batch dimension value for each response. The shape needs to be
  // initialized lazily because the SupportsFirstDimBatching function
  // cannot be used until the model is completely loaded.
  if (!shape_initialized_) {
    bool supports_first_dim_batching;
    RETURN_IF_ERROR(SupportsFirstDimBatching(&supports_first_dim_batching));
    if (supports_first_dim_batching) {
      shape_.push_back(-1);
    }

    shape_.insert(shape_.end(), nb_shape_.begin(), nb_shape_.end());
    shape_initialized_ = true;
  }

  shape = shape_;

  return nullptr;  // success
}

TRITONSERVER_Error*
ModelState::ValidateModelConfig()
{
  // If verbose logging is enabled, dump the model's configuration as
  // JSON into the console output.
  if (TRITONSERVER_LogIsEnabled(TRITONSERVER_LOG_VERBOSE)) {
    common::TritonJson::WriteBuffer buffer;
    RETURN_IF_ERROR(ModelConfig().PrettyWrite(&buffer));
    LOG_MESSAGE(
        TRITONSERVER_LOG_VERBOSE,
        (std::string("model configuration:\n") + buffer.Contents()).c_str());
  }

  // ModelConfig is the model configuration as a TritonJson
  // object. Use the TritonJson utilities to parse the JSON and
  // determine if the configuration is supported by this backend.
  common::TritonJson::Value inputs, outputs;
  RETURN_IF_ERROR(ModelConfig().MemberAsArray("input", &inputs));
  RETURN_IF_ERROR(ModelConfig().MemberAsArray("output", &outputs));

  // The model must have exactly 1 input and 1 output.
  RETURN_ERROR_IF_FALSE(
      inputs.ArraySize() == 1, TRITONSERVER_ERROR_INVALID_ARG,
      std::string("model configuration must have 1 input"));
  RETURN_ERROR_IF_FALSE(
      outputs.ArraySize() == 1, TRITONSERVER_ERROR_INVALID_ARG,
      std::string("model configuration must have 1 output"));

  common::TritonJson::Value input, output;
  RETURN_IF_ERROR(inputs.IndexAsObject(0, &input));
  RETURN_IF_ERROR(outputs.IndexAsObject(0, &output));

  // Record the input and output name in the model state.
  const char* input_name;
  size_t input_name_len;
  RETURN_IF_ERROR(input.MemberAsString("name", &input_name, &input_name_len));
  input_name_ = std::string(input_name);

  const char* output_name;
  size_t output_name_len;
  RETURN_IF_ERROR(
      output.MemberAsString("name", &output_name, &output_name_len));
  output_name_ = std::string(output_name);

  // Input and output must have same datatype
  std::string input_dtype, output_dtype;
  RETURN_IF_ERROR(input.MemberAsString("data_type", &input_dtype));
  RETURN_IF_ERROR(output.MemberAsString("data_type", &output_dtype));
  RETURN_ERROR_IF_FALSE(
      input_dtype == output_dtype, TRITONSERVER_ERROR_INVALID_ARG,
      std::string("expected input and output datatype to match, got ") +
          input_dtype + " and " + output_dtype);
  datatype_ = ModelConfigDataTypeToTritonServerDataType(input_dtype);

  // Input and output must have same shape. Reshape is not supported
  // on either input or output so flag an error is the model
  // configuration uses it.
  triton::common::TritonJson::Value reshape;
  RETURN_ERROR_IF_TRUE(
      input.Find("reshape", &reshape), TRITONSERVER_ERROR_UNSUPPORTED,
      std::string("reshape not supported for input tensor"));
  RETURN_ERROR_IF_TRUE(
      output.Find("reshape", &reshape), TRITONSERVER_ERROR_UNSUPPORTED,
      std::string("reshape not supported for output tensor"));

  std::vector<int64_t> input_shape, output_shape;
  RETURN_IF_ERROR(backend::ParseShape(input, "dims", &input_shape));
  RETURN_IF_ERROR(backend::ParseShape(output, "dims", &output_shape));

  RETURN_ERROR_IF_FALSE(
      input_shape == output_shape, TRITONSERVER_ERROR_INVALID_ARG,
      std::string("expected input and output shape to match, got ") +
          backend::ShapeToString(input_shape) + " and " +
          backend::ShapeToString(output_shape));

  nb_shape_ = input_shape;

  return nullptr;  // success
}


extern "C" {

// Triton calls TRITONBACKEND_ModelInitialize when a model is loaded
// to allow the backend to create any state associated with the model,
// and to also examine the model configuration to determine if the
// configuration is suitable for the backend. Any errors reported by
// this function will prevent the model from loading.
//
TRITONSERVER_Error*
TRITONBACKEND_ModelInitialize(TRITONBACKEND_Model* model)
{
  // Create a ModelState object and associate it with the
  // TRITONBACKEND_Model. If anything goes wrong with initialization
  // of the model state then an error is returned and Triton will fail
  // to load the model.
  ModelState* model_state;
  RETURN_IF_ERROR(ModelState::Create(model, &model_state));
  RETURN_IF_ERROR(
      TRITONBACKEND_ModelSetState(model, reinterpret_cast<void*>(model_state)));

  return nullptr;  // success
}

// Triton calls TRITONBACKEND_ModelFinalize when a model is no longer
// needed. The backend should cleanup any state associated with the
// model. This function will not be called until all model instances
// of the model have been finalized.
//
TRITONSERVER_Error*
TRITONBACKEND_ModelFinalize(TRITONBACKEND_Model* model)
{
  void* vstate;
  RETURN_IF_ERROR(TRITONBACKEND_ModelState(model, &vstate));
  ModelState* model_state = reinterpret_cast<ModelState*>(vstate);
  delete model_state;

  return nullptr;  // success
}

}  // extern "C"

/////////////

typedef std::unique_ptr<triton::backend::adsbrain::AdsbrainInferenceModel> (
    *createAdsbrainInferenceModel)();

//
// ModelInstanceState
//
// State associated with a model instance. An object of this class is
// created and associated with each
// TRITONBACKEND_ModelInstance. ModelInstanceState is derived from
// BackendModelInstance class provided in the backend utilities that
// provides many common functions.
//
class ModelInstanceState : public BackendModelInstance {
 public:
  static TRITONSERVER_Error* Create(
      ModelState* model_state,
      TRITONBACKEND_ModelInstance* triton_model_instance,
      ModelInstanceState** state);
  virtual ~ModelInstanceState() { dlclose(model_lib_handle_); }

  // Get the state of the model that corresponds to this instance.
  ModelState* StateForModel() const { return model_state_; }

  std::vector<std::string> RunInference(
      const std::vector<std::string>& requests)
  {
    return adsbrain_model_->RunInference(requests);
  }

  bool SetStringOutputBuffer(
      const std::string& name, const char* content, const size_t* offsets,
      std::vector<int64_t>* batchn_shape, TRITONBACKEND_Request** requests,
      const uint32_t request_count,
      std::vector<TRITONBACKEND_Response*>* responses);

  bool SetStringStateBuffer(
      const std::string& name, const char* content, const size_t* offsets,
      std::vector<int64_t>* batchn_shape, TRITONBACKEND_Request** requests,
      const uint32_t request_count,
      std::vector<TRITONBACKEND_Response*>* responses);

  bool SetStringBuffer(
      const std::string& name, const char* content, const size_t* offsets,
      std::vector<int64_t>* batchn_shape, TRITONBACKEND_Request** requests,
      const uint32_t request_count,
      std::vector<TRITONBACKEND_Response*>* responses, bool state);

  TRITONSERVER_Error* SetResponses(
      const std::vector<std::string>& response_data,
      const std::vector<TRITONBACKEND_Response*>& responses,
      const std::string& output_name, bool* cuda_copy);

 private:
  ModelInstanceState(
      ModelState* model_state,
      TRITONBACKEND_ModelInstance* triton_model_instance)
      : BackendModelInstance(model_state, triton_model_instance),
        model_state_(model_state)
  {
    auto adsbrain_model_configurations = model_state_->GetModelConfig();

    model_lib_handle_ = dlopen(
        adsbrain_model_configurations["model_lib_path"].c_str(), RTLD_NOW);
    if (!model_lib_handle_) {
      throw std::invalid_argument(
          "Cannot open library: " + std::string(dlerror()));
    }

    const char* func_name = "CreateInferenceModel";
    createAdsbrainInferenceModel create_model_func_ =
        (createAdsbrainInferenceModel)dlsym(model_lib_handle_, func_name);
    if (!create_model_func_) {
      throw std::invalid_argument(
          "Cannot load symbol CreateInferenceModel: " + std::string(dlerror()));
    }

    adsbrain_model_ = (*create_model_func_)();
    adsbrain_model_->Initialize(adsbrain_model_configurations);
  }

  ModelState* model_state_;
  std::unique_ptr<AdsbrainInferenceModel> adsbrain_model_;
  void* model_lib_handle_;
};

TRITONSERVER_Error*
ModelInstanceState::Create(
    ModelState* model_state, TRITONBACKEND_ModelInstance* triton_model_instance,
    ModelInstanceState** state)
{
  try {
    *state = new ModelInstanceState(model_state, triton_model_instance);
  }
  catch (const BackendModelInstanceException& ex) {
    RETURN_ERROR_IF_TRUE(
        ex.err_ == nullptr, TRITONSERVER_ERROR_INTERNAL,
        std::string("unexpected nullptr in BackendModelInstanceException"));
    RETURN_IF_ERROR(ex.err_);
  }

  return nullptr;  // success
}

bool
ModelInstanceState::SetStringStateBuffer(
    const std::string& name, const char* content, const size_t* offsets,
    std::vector<int64_t>* batchn_shape, TRITONBACKEND_Request** requests,
    const uint32_t request_count,
    std::vector<TRITONBACKEND_Response*>* responses)
{
  return SetStringBuffer(
      name, content, offsets, batchn_shape, requests, request_count, responses,
      true /* state */);
}

// TODO: we assume 1) the model only has one output; 2) the output is in the
// shape of [1]
TRITONSERVER_Error*
ModelInstanceState::SetResponses(
    const std::vector<std::string>& response_data,
    const std::vector<TRITONBACKEND_Response*>& responses,
    const std::string& output_name, bool* cuda_copy)
{
  *cuda_copy = false;
  TRITONSERVER_Error* err;
  std::vector<int64_t> shape = {1};
  for (size_t i = 0; i < responses.size(); ++i) {
    auto& response = responses[i];
    const std::string& data_str = response_data[i];
    TRITONBACKEND_Output* response_output;
    err = TRITONBACKEND_ResponseOutput(
        response, &response_output, output_name.c_str(),
        TRITONSERVER_TYPE_BYTES, shape.data(), shape.size());
    if (err != nullptr) {
      return err;
    }
    TRITONSERVER_MemoryType actual_memory_type = TRITONSERVER_MEMORY_CPU_PINNED;
    int64_t actual_memory_type_id = 0;
    void* buffer;
    err = TRITONBACKEND_OutputBuffer(
        response_output, &buffer, data_str.size() + sizeof(uint32_t),
        &actual_memory_type, &actual_memory_type_id);
    if (err != nullptr) {
      return err;
    }
    bool cuda_used = false;
    size_t copied_byte_size = 0;
    // Copy the data length
    const uint32_t len = data_str.size();
    err = CopyBuffer(
        output_name, TRITONSERVER_MEMORY_CPU /* src_memory_type */,
        0 /* src_memory_type_id */, actual_memory_type, actual_memory_type_id,
        sizeof(uint32_t), static_cast<const void*>(&len),
        static_cast<char*>(buffer) + copied_byte_size, stream_, &cuda_used);
    if (err != nullptr) {
      return err;
    }
    *cuda_copy |= cuda_used;
    copied_byte_size += sizeof(uint32_t);
    // Copy raw string content
    err = CopyBuffer(
        output_name, TRITONSERVER_MEMORY_CPU /* src_memory_type */,
        0 /* src_memory_type_id */, actual_memory_type, actual_memory_type_id,
        len, static_cast<const void*>(data_str.c_str()),
        static_cast<char*>(buffer) + copied_byte_size, stream_, &cuda_used);
    if (err != nullptr) {
      return err;
    }

    *cuda_copy |= cuda_used;
    copied_byte_size += len;
  }

  return nullptr;
}

bool
ModelInstanceState::SetStringOutputBuffer(
    const std::string& name, const char* content, const size_t* offsets,
    std::vector<int64_t>* batchn_shape, TRITONBACKEND_Request** requests,
    const uint32_t request_count,
    std::vector<TRITONBACKEND_Response*>* responses)
{
  return SetStringBuffer(
      name, content, offsets, batchn_shape, requests, request_count, responses,
      false /* state */);
}

bool
ModelInstanceState::SetStringBuffer(
    const std::string& name, const char* content, const size_t* offsets,
    std::vector<int64_t>* batchn_shape, TRITONBACKEND_Request** requests,
    const uint32_t request_count,
    std::vector<TRITONBACKEND_Response*>* responses, bool state)
{
  size_t element_idx = 0;
  bool cuda_copy = false;
  for (size_t ridx = 0; ridx < request_count; ++ridx) {
    const auto& request = requests[ridx];
    auto& response = (*responses)[ridx];

    // batchn_shape holds the shape of the entire tensor batch. When
    // batching is enabled override the first batch dimension with each
    // requests batch size (reusing for efficiency).
    if (model_state_->MaxBatchSize() > 0) {
      TRITONBACKEND_Input* input;
      TRITONBACKEND_RequestInputByIndex(request, 0 /* index */, &input);
      const int64_t* shape;
      TRITONBACKEND_InputProperties(
          input, nullptr, nullptr, &shape, nullptr, nullptr, nullptr);
      (*batchn_shape)[0] = shape[0];
    }

    const size_t expected_element_cnt = GetElementCount(*batchn_shape);

    // If 'request' requested this output then copy it from
    // 'content'. If it did not request this output then just skip it
    // in the 'content'.
    bool need_output = false;
    if (!state) {
      if (response != nullptr) {
        uint32_t output_count;
        RESPOND_AND_SET_NULL_IF_ERROR(
            &response,
            TRITONBACKEND_RequestOutputCount(request, &output_count));
        if (response != nullptr) {
          for (uint32_t output_idx = 0; output_idx < output_count;
               output_idx++) {
            const char* req_output_name;
            RESPOND_AND_SET_NULL_IF_ERROR(
                &response, TRITONBACKEND_RequestOutputName(
                               request, output_idx, &req_output_name));
            if ((response != nullptr) && (req_output_name == name)) {
              need_output = true;
              break;
            }
          }
        }
      }
    } else {
      // need_output must be always set to true for state tensors.
      need_output = true;
    }

    if (need_output) {
      TRITONSERVER_Error* err;
      TRITONBACKEND_Output* response_output;
      TRITONBACKEND_State* response_state;
      if (!state) {
        err = TRITONBACKEND_ResponseOutput(
            response, &response_output, name.c_str(), TRITONSERVER_TYPE_BYTES,
            batchn_shape->data(), batchn_shape->size());
      } else {
        err = TRITONBACKEND_StateNew(
            &response_state, request, name.c_str(), TRITONSERVER_TYPE_BYTES,
            batchn_shape->data(), batchn_shape->size());
      }
      if (err == nullptr) {
        // Calculate expected byte size in advance using string offsets
        const size_t data_byte_size =
            offsets[element_idx + expected_element_cnt] - offsets[element_idx];
        const size_t expected_byte_size =
            data_byte_size + sizeof(uint32_t) * expected_element_cnt;

        TRITONSERVER_MemoryType actual_memory_type =
            TRITONSERVER_MEMORY_CPU_PINNED;
        int64_t actual_memory_type_id = 0;
        void* buffer;
        if (!state) {
          err = TRITONBACKEND_OutputBuffer(
              response_output, &buffer, expected_byte_size, &actual_memory_type,
              &actual_memory_type_id);
        } else {
          err = TRITONBACKEND_StateBuffer(
              response_state, &buffer, expected_byte_size, &actual_memory_type,
              &actual_memory_type_id);
        }
        if (err == nullptr) {
          bool cuda_used = false;
          size_t copied_byte_size = 0;
          for (size_t e = 0; e < expected_element_cnt; ++e) {
            const uint32_t len =
                offsets[element_idx + e + 1] - offsets[element_idx + e];
            // // Prepend size of the string
            err = CopyBuffer(
                name, TRITONSERVER_MEMORY_CPU /* src_memory_type */,
                0 /* src_memory_type_id */, actual_memory_type,
                actual_memory_type_id, sizeof(uint32_t),
                static_cast<const void*>(&len),
                static_cast<char*>(buffer) + copied_byte_size, stream_,
                &cuda_used);
            if (err != nullptr) {
              break;
            }

            cuda_copy |= cuda_used;
            copied_byte_size += sizeof(uint32_t);

            // Copy raw string content
            err = CopyBuffer(
                name, TRITONSERVER_MEMORY_CPU /* src_memory_type */,
                0 /* src_memory_type_id */, actual_memory_type,
                actual_memory_type_id, len, content + offsets[element_idx + e],
                static_cast<char*>(buffer) + copied_byte_size, stream_,
                &cuda_used);
            if (err != nullptr) {
              break;
            }

            cuda_copy |= cuda_used;
            copied_byte_size += len;
          }
        }
      }

      RESPOND_AND_SET_NULL_IF_ERROR(&response, err);
      if (state) {
        RESPOND_AND_SET_NULL_IF_ERROR(
            &response, TRITONBACKEND_StateUpdate(response_state));
      }
    }

    element_idx += expected_element_cnt;
  }

  return cuda_copy;
}

extern "C" {

// Triton calls TRITONBACKEND_ModelInstanceInitialize when a model
// instance is created to allow the backend to initialize any state
// associated with the instance.
//
TRITONSERVER_Error*
TRITONBACKEND_ModelInstanceInitialize(TRITONBACKEND_ModelInstance* instance)
{
  // Get the model state associated with this instance's model.
  TRITONBACKEND_Model* model;
  RETURN_IF_ERROR(TRITONBACKEND_ModelInstanceModel(instance, &model));

  void* vmodelstate;
  RETURN_IF_ERROR(TRITONBACKEND_ModelState(model, &vmodelstate));
  ModelState* model_state = reinterpret_cast<ModelState*>(vmodelstate);

  // Create a ModelInstanceState object and associate it with the
  // TRITONBACKEND_ModelInstance.
  ModelInstanceState* instance_state;
  RETURN_IF_ERROR(
      ModelInstanceState::Create(model_state, instance, &instance_state));
  RETURN_IF_ERROR(TRITONBACKEND_ModelInstanceSetState(
      instance, reinterpret_cast<void*>(instance_state)));

  return nullptr;  // success
}

// Triton calls TRITONBACKEND_ModelInstanceFinalize when a model
// instance is no longer needed. The backend should cleanup any state
// associated with the model instance.
//
TRITONSERVER_Error*
TRITONBACKEND_ModelInstanceFinalize(TRITONBACKEND_ModelInstance* instance)
{
  void* vstate;
  RETURN_IF_ERROR(TRITONBACKEND_ModelInstanceState(instance, &vstate));
  ModelInstanceState* instance_state =
      reinterpret_cast<ModelInstanceState*>(vstate);
  delete instance_state;

  return nullptr;  // success
}

}  // extern "C"

/////////////

extern "C" {

// When Triton calls TRITONBACKEND_ModelInstanceExecute it is required
// that a backend create a response for each request in the batch. A
// response may be the output tensors required for that request or may
// be an error that is returned in the response.
//
TRITONSERVER_Error*
TRITONBACKEND_ModelInstanceExecute(
    TRITONBACKEND_ModelInstance* instance, TRITONBACKEND_Request** requests,
    const uint32_t request_count)
{
  // Collect various timestamps during the execution of this batch or
  // requests. These values are reported below before returning from
  // the function.

  uint64_t exec_start_ns = 0;
  SET_TIMESTAMP(exec_start_ns);

  // Triton will not call this function simultaneously for the same
  // 'instance'. But since this backend could be used by multiple
  // instances from multiple models the implementation needs to handle
  // multiple calls to this function at the same time (with different
  // 'instance' objects). Best practice for a high-performance
  // implementation is to avoid introducing mutex/lock and instead use
  // only function-local and model-instance-specific state.
  ModelInstanceState* instance_state;
  RETURN_IF_ERROR(TRITONBACKEND_ModelInstanceState(
      instance, reinterpret_cast<void**>(&instance_state)));
  ModelState* model_state = instance_state->StateForModel();

  // 'responses' is initialized as a parallel array to 'requests',
  // with one TRITONBACKEND_Response object for each
  // TRITONBACKEND_Request object. If something goes wrong while
  // creating these response objects, the backend simply returns an
  // error from TRITONBACKEND_ModelInstanceExecute, indicating to
  // Triton that this backend did not create or send any responses and
  // so it is up to Triton to create and send an appropriate error
  // response for each request. RETURN_IF_ERROR is one of several
  // useful macros for error handling that can be found in
  // backend_common.h.

  std::vector<TRITONBACKEND_Response*> responses;
  responses.reserve(request_count);
  for (uint32_t r = 0; r < request_count; ++r) {
    TRITONBACKEND_Request* request = requests[r];
    TRITONBACKEND_Response* response;
    RETURN_IF_ERROR(TRITONBACKEND_ResponseNew(&response, request));
    responses.push_back(response);
  }

  // At this point, the backend takes ownership of 'requests', which
  // means that it is responsible for sending a response for every
  // request. From here, even if something goes wrong in processing,
  // the backend must return 'nullptr' from this function to indicate
  // success. Any errors and failures must be communicated via the
  // response objects.
  //
  // To simplify error handling, the backend utilities manage
  // 'responses' in a specific way and it is recommended that backends
  // follow this same pattern. When an error is detected in the
  // processing of a request, an appropriate error response is sent
  // and the corresponding TRITONBACKEND_Response object within
  // 'responses' is set to nullptr to indicate that the
  // request/response has already been handled and no futher processing
  // should be performed for that request. Even if all responses fail,
  // the backend still allows execution to flow to the end of the
  // function so that statistics are correctly reported by the calls
  // to TRITONBACKEND_ModelInstanceReportStatistics and
  // TRITONBACKEND_ModelInstanceReportBatchStatistics.
  // RESPOND_AND_SET_NULL_IF_ERROR, and
  // RESPOND_ALL_AND_SET_NULL_IF_ERROR are macros from
  // backend_common.h that assist in this management of response
  // objects.

  // The backend could iterate over the 'requests' and process each
  // one separately. But for performance reasons it is usually
  // preferred to create batched input tensors that are processed
  // simultaneously. This is especially true for devices like GPUs
  // that are capable of exploiting the large amount parallelism
  // exposed by larger data sets.
  //
  // The backend utilities provide a "collector" to facilitate this
  // batching process. The 'collector's ProcessTensor function will
  // combine a tensor's value from each request in the batch into a
  // single contiguous buffer. The buffer can be provided by the
  // backend or 'collector' can create and manage it. In this backend,
  // there is not a specific buffer into which the batch should be
  // created, so use ProcessTensor arguments that cause collector to
  // manage it.

  BackendInputCollector collector(
      requests, request_count, &responses, model_state->TritonMemoryManager(),
      false /* pinned_enabled */, instance_state->CudaStream() /* stream*/);

  // To instruct ProcessTensor to "gather" the entire batch of input
  // tensors into a single contiguous buffer in CPU memory, set the
  // "allowed input types" to be the CPU ones (see tritonserver.h in
  // the triton-inference-server/core repo for allowed memory types).
  std::vector<std::pair<TRITONSERVER_MemoryType, int64_t>> allowed_input_types =
      {{TRITONSERVER_MEMORY_CPU_PINNED, 0}, {TRITONSERVER_MEMORY_CPU, 0}};

  const char* input_buffer;
  size_t input_buffer_byte_size;
  TRITONSERVER_MemoryType input_buffer_memory_type;
  int64_t input_buffer_memory_type_id;

  TRITONSERVER_Error* err = collector.ProcessTensor(
      model_state->InputTensorName().c_str(), nullptr /* existing_buffer */,
      0 /* existing_buffer_byte_size */, allowed_input_types, &input_buffer,
      &input_buffer_byte_size, &input_buffer_memory_type,
      &input_buffer_memory_type_id);
  if (err != nullptr) {
    LOG_MESSAGE(TRITONSERVER_LOG_ERROR, TRITONSERVER_ErrorMessage(err));
  }

  RESPOND_ALL_AND_SET_NULL_IF_ERROR(responses, request_count, err);

  // Finalize the collector. If 'true' is returned, 'input_buffer'
  // will not be valid until the backend synchronizes the CUDA
  // stream or event that was used when creating the collector. For
  // this backend, GPU is not supported and so no CUDA sync should
  // be needed; so if 'true' is returned simply log an error.
  const bool need_cuda_input_sync = collector.Finalize();
  if (need_cuda_input_sync) {
    LOG_MESSAGE(
        TRITONSERVER_LOG_ERROR,
        "Adsbrain backend: unexpected CUDA sync required by collector");
  }

  uint64_t compute_start_ns = 0;
  SET_TIMESTAMP(compute_start_ns);

  LOG_MESSAGE(
      TRITONSERVER_LOG_VERBOSE,
      (std::string("model ") + model_state->Name() + ": requests in batch " +
       std::to_string(request_count))
          .c_str());

  LOG_MESSAGE(
      TRITONSERVER_LOG_VERBOSE,
      (std::string(model_state->InputTensorName() + " value: ") +
       std::string(input_buffer, input_buffer_byte_size))
          .c_str());

  bool cuda_copy = false;
  // If everything works correctly, extract the batched requests and run
  // inference.
  if (err == nullptr) {
    // 'input_buffer' contains the batched requests. Decode them to seperate
    // requests.
    std::vector<std::string> request_strs;
    request_strs.reserve(request_count);

    const char* cur_input_str_ptr = input_buffer;
    for (uint32_t i = 0; i < request_count; ++i) {
      uint32_t input_str_size = *((uint32_t*)cur_input_str_ptr);
      cur_input_str_ptr += sizeof(uint32_t);
      request_strs.push_back(std::string(cur_input_str_ptr, input_str_size));
      cur_input_str_ptr += input_str_size;
    }

    std::vector<std::string> response_strs;

    try {
      response_strs = instance_state->RunInference(request_strs);
    }
    catch (const std::exception& ex) {
      std::string err_msg = "Model " + model_state->Name() +
                            ": failed to run inference: " + ex.what();
      LOG_MESSAGE(TRITONSERVER_LOG_ERROR, err_msg.c_str());
      err = TRITONSERVER_ErrorNew(TRITONSERVER_ERROR_INTERNAL, err_msg.c_str());
      // `err` will be released by the below macro
      RESPOND_ALL_AND_SET_NULL_IF_ERROR(responses, request_count, err);
    }

    if (response_strs.size() != request_strs.size()) {
      std::string err_msg =
          "Molde inference expected " + std::to_string(request_strs.size()) +
          " response strings, but got " + std::to_string(response_strs.size());
      LOG_MESSAGE(TRITONSERVER_LOG_ERROR, err_msg.c_str());
      err = TRITONSERVER_ErrorNew(TRITONSERVER_ERROR_INTERNAL, err_msg.c_str());
      // `err` will be released by the below macro
      RESPOND_ALL_AND_SET_NULL_IF_ERROR(responses, request_count, err);
    } else {
      err = instance_state->SetResponses(
          response_strs, responses, model_state->OutputTensorName(),
          &cuda_copy);
      if (err != nullptr) {
        LOG_MESSAGE(TRITONSERVER_LOG_ERROR, TRITONSERVER_ErrorMessage(err));
      }
      RESPOND_ALL_AND_SET_NULL_IF_ERROR(responses, responses.size(), err);
    }
  }

  uint64_t compute_end_ns = 0;
  SET_TIMESTAMP(compute_end_ns);


#ifdef TRITON_ENABLE_GPU
  if (cuda_copy) {
    cudaStreamSynchronize(instance_state->CudaStream());
  }
#elif
  LOG_MESSAGE(
      TRITONSERVER_LOG_ERROR,
      "'recommended' backend: unexpected CUDA sync required by responder");
}
#endif  // TRITON_ENABLE_GPU

  // Finalize the responder. If 'true' is returned, the output
  // tensors' data will not be valid until the backend synchronizes
  // the CUDA stream or event that was used when creating the
  // responder. For this backend, GPU is not supported and so no CUDA
  // sync should be needed; so if 'true' is returned simply log an
  // error.
  // const bool need_cuda_output_sync = responder.Finalize();
  // if (need_cuda_output_sync) {
  //   LOG_MESSAGE(
  //       TRITONSERVER_LOG_ERROR,
  //       "'recommended' backend: unexpected CUDA sync required by responder");
  // }

  // Send all the responses that haven't already been sent because of
  // an earlier error.
  for (auto& response : responses) {
    if (response != nullptr) {
      LOG_IF_ERROR(
          TRITONBACKEND_ResponseSend(
              response, TRITONSERVER_RESPONSE_COMPLETE_FINAL, nullptr),
          "failed to send response");
    }
  }

  uint64_t exec_end_ns = 0;
  SET_TIMESTAMP(exec_end_ns);

#ifdef TRITON_ENABLE_STATS
  // For batch statistics need to know the total batch size of the
  // requests. This is not necessarily just the number of requests,
  // because if the model supports batching then any request can be a
  // batched request itself.
  size_t total_batch_size = 0;
  bool supports_first_dim_batching;
  RESPOND_ALL_AND_SET_NULL_IF_ERROR(
      responses, request_count,
      model_state->SupportsFirstDimBatching(&supports_first_dim_batching));
  if (!supports_first_dim_batching) {
    total_batch_size = request_count;
  } else {
    for (uint32_t r = 0; r < request_count; ++r) {
      auto& request = requests[r];
      TRITONBACKEND_Input* input = nullptr;
      LOG_IF_ERROR(
          TRITONBACKEND_RequestInputByIndex(request, 0 /* index */, &input),
          "failed getting request input");
      if (input != nullptr) {
        const int64_t* shape = nullptr;
        LOG_IF_ERROR(
            TRITONBACKEND_InputProperties(
                input, nullptr, nullptr, &shape, nullptr, nullptr, nullptr),
            "failed getting input properties");
        if (shape != nullptr) {
          total_batch_size += shape[0];
        }
      }
    }
  }
#else
(void)exec_start_ns;
(void)exec_end_ns;
(void)compute_start_ns;
(void)compute_end_ns;
#endif  // TRITON_ENABLE_STATS

  // Report statistics for each request, and then release the request.
  for (uint32_t r = 0; r < request_count; ++r) {
    auto& request = requests[r];

#ifdef TRITON_ENABLE_STATS
    LOG_IF_ERROR(
        TRITONBACKEND_ModelInstanceReportStatistics(
            instance_state->TritonModelInstance(), request,
            (responses[r] != nullptr) /* success */, exec_start_ns,
            compute_start_ns, compute_end_ns, exec_end_ns),
        "failed reporting request statistics");
#endif  // TRITON_ENABLE_STATS

    LOG_IF_ERROR(
        TRITONBACKEND_RequestRelease(request, TRITONSERVER_REQUEST_RELEASE_ALL),
        "failed releasing request");
  }

#ifdef TRITON_ENABLE_STATS
  // Report batch statistics.
  LOG_IF_ERROR(
      TRITONBACKEND_ModelInstanceReportBatchStatistics(
          instance_state->TritonModelInstance(), total_batch_size,
          exec_start_ns, compute_start_ns, compute_end_ns, exec_end_ns),
      "failed reporting batch request statistics");
#endif  // TRITON_ENABLE_STATS

  return nullptr;  // success
}

}  // extern "C"

}}}  // namespace triton::backend::adsbrain
