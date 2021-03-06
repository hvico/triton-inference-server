// Copyright (c) 2020, NVIDIA CORPORATION. All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions
// are met:
//  * Redistributions of source code must retain the above copyright
//    notice, this list of conditions and the following disclaimer.
//  * Redistributions in binary form must reproduce the above copyright
//    notice, this list of conditions and the following disclaimer in the
//    documentation and/or other materials provided with the distribution.
//  * Neither the name of NVIDIA CORPORATION nor the names of its
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

#include "src/backends/backend/triton_model.h"

#include <vector>
#include "src/backends/backend/triton_model_instance.h"
#include "src/backends/backend/tritonbackend.h"
#include "src/core/filesystem.h"
#include "src/core/logging.h"
#include "src/core/model_config_utils.h"
#include "src/core/server_message.h"
#include "src/core/tritonserver.h"

namespace nvidia { namespace inferenceserver {

namespace {

Status
BackendConfiguration(
    const BackendCmdlineConfig& config, const std::string& key,
    std::string* val)
{
  for (const auto& pr : config) {
    if (pr.first == key) {
      *val = pr.second;
      return Status::Success;
    }
  }

  return Status(
      Status::Code::INTERNAL,
      std::string("unable to find common backend configuration for '") + key +
          "'");
}

Status
ParseStringToDouble(const std::string& str, double* val)
{
  try {
    *val = std::stod(str);
  }
  catch (...) {
    return Status(
        Status::Code::INTERNAL,
        "unable to parse common backend configuration as double");
  }

  return Status::Success;
}

}  // namespace

Status
TritonModel::Create(
    InferenceServer* server, const std::string& model_repository_path,
    const BackendCmdlineConfigMap& backend_cmdline_config_map,
    const std::string& model_name, const int64_t version,
    const inference::ModelConfig& model_config,
    std::unique_ptr<TritonModel>* model)
{
  model->reset();

  // Get some internal configuration values needed for initialization.
  std::string backend_dir;
#ifdef TRITON_ENABLE_GPU
  double min_compute_capability = TRITON_MIN_COMPUTE_CAPABILITY;
#else
  double min_compute_capability = 0;
#endif  // TRITON_ENABLE_GPU
  {
    const auto& itr = backend_cmdline_config_map.find(std::string());
    if (itr == backend_cmdline_config_map.end()) {
      return Status(
          Status::Code::INTERNAL,
          "unable to find common backend configuration");
    }

    RETURN_IF_ERROR(
        BackendConfiguration(itr->second, "backend-directory", &backend_dir));

    std::string min_compute_capability_str;
    RETURN_IF_ERROR(BackendConfiguration(
        itr->second, "min-compute-capability", &min_compute_capability_str));
    RETURN_IF_ERROR(ParseStringToDouble(
        min_compute_capability_str, &min_compute_capability));
  }

  // The model configuration must specify a backend. The name of the
  // corresponding shared library must be libtriton_<backend>.so.
  if (model_config.backend().empty()) {
    return Status(
        Status::Code::INVALID_ARG,
        "must specify 'backend' for '" + model_config.name() + "'");
  }

  // Localize the content of the model repository corresponding to
  // 'model_name'. This model holds a handle to the localized content
  // so that it persists as long as the model is loaded.
  std::shared_ptr<LocalizedDirectory> localized_model_dir;
  RETURN_IF_ERROR(LocalizeDirectory(
      JoinPath({model_repository_path, model_name}), &localized_model_dir));

  const std::string backend_libname =
      "libtriton_" + model_config.backend() + ".so";

  // Get the path to the backend shared library. Search path is
  // version directory, model directory, global backend directory.
  const auto model_path = localized_model_dir->Path();
  const auto version_path = JoinPath({model_path, std::to_string(version)});
  const std::string global_path =
      JoinPath({backend_dir, model_config.backend()});
  const std::vector<std::string> search_paths = {version_path, model_path,
                                                 global_path};

  std::string backend_libpath;
  for (const auto& path : search_paths) {
    const auto full_path = JoinPath({path, backend_libname});
    bool exists = false;
    RETURN_IF_ERROR(FileExists(full_path, &exists));
    if (exists) {
      backend_libpath = full_path;
      break;
    }
  }

  if (backend_libpath.empty()) {
    return Status(
        Status::Code::INVALID_ARG, "unable to find '" + backend_libname +
                                       "' for model '" + model_config.name() +
                                       "', searched: " + version_path + ", " +
                                       model_path + ", " + global_path);
  }

  // Find/create the backend
  BackendCmdlineConfig empty_backend_cmdline_config;
  const BackendCmdlineConfig* config;
  const auto& itr = backend_cmdline_config_map.find(model_config.backend());
  if (itr == backend_cmdline_config_map.end()) {
    config = &empty_backend_cmdline_config;
  } else {
    config = &itr->second;
  }

  std::shared_ptr<TritonBackend> backend;
  RETURN_IF_ERROR(TritonBackendManager::CreateBackend(
      model_config.backend(), backend_libpath, *config, &backend));

  // Create and initialize the model.
  std::unique_ptr<TritonModel> local_model(new TritonModel(
      server, localized_model_dir, backend, min_compute_capability));
  RETURN_IF_ERROR(
      local_model->Init(version_path, model_config, "" /* platform */));

  TritonModel* raw_local_model = local_model.get();

  // Model initialization is optional... The TRITONBACKEND_Model
  // object is this TritonModel object.
  if (backend->ModelInitFn() != nullptr) {
    RETURN_IF_TRITONSERVER_ERROR(backend->ModelInitFn()(
        reinterpret_cast<TRITONBACKEND_Model*>(raw_local_model)));
  }

  // Create and initialize the model instances for this model.
  RETURN_IF_ERROR(TritonModelInstance::CreateInstances(
      raw_local_model, model_config, &local_model->instances_));

  // Create a scheduler with 1 thread per instance. The backend is
  // already initialized so there is no need to have the scheduler
  // thread call any initialization.
  RETURN_IF_ERROR(local_model->SetConfiguredScheduler(
      local_model->instances_.size() /* runner_cnt */,
      /* Initialization callback */
      [](uint32_t runner_idx) -> Status { return Status::Success; },
      /* Run callback */
      [raw_local_model, backend](
          uint32_t runner_idx,
          std::vector<std::unique_ptr<InferenceRequest>>&& requests) {
        // Use a thread local vector to avoid needing to malloc each
        // time an inference is run.
        thread_local std::vector<TRITONBACKEND_Request*> triton_requests(1024);
        triton_requests.clear();
        for (auto& r : requests) {
          triton_requests.push_back(
              reinterpret_cast<TRITONBACKEND_Request*>(r.release()));
        }

        // We don't want the backend used by this model to unload
        // while exec_fn is running (can happen if model is unloaded
        // during the request and then that request is released in
        // exec_fn as the last reference to the model). So we hold a
        // copy of the backend here... This convoluted flow will be
        // cleaned up once legacy InferenceBackend is replaced with
        // TritonModel.
        std::shared_ptr<TritonBackend> backendx = backend;

        TRITONBACKEND_ModelInstance* triton_model_instance =
            reinterpret_cast<TRITONBACKEND_ModelInstance*>(
                raw_local_model->instances_[runner_idx].get());
        TritonBackend::TritonModelInstanceExecFn_t inst_exec_fn =
            backend->ModelInstanceExecFn();

        // If there is an error then we retain ownership of 'requests'
        // and must send error responses.
        TRITONSERVER_Error* err = inst_exec_fn(
            triton_model_instance, &triton_requests[0], triton_requests.size());
        if (err != nullptr) {
          Status status = Status(
              TritonCodeToStatusCode(TRITONSERVER_ErrorCode(err)),
              TRITONSERVER_ErrorMessage(err));
          for (TRITONBACKEND_Request* tr : triton_requests) {
            std::unique_ptr<InferenceRequest> ur(
                reinterpret_cast<InferenceRequest*>(tr));
            InferenceRequest::RespondIfError(
                ur, status, true /* release_requests */);
          }

          TRITONSERVER_ErrorDelete(err);
        }

        return Status::Success;
      }));

  *model = std::move(local_model);
  return Status::Success;
}

void
TritonModel::WarmUp(uint32_t runner_idx, WarmupData& sample)
{
  std::vector<TRITONBACKEND_Request*> triton_requests(1024);
  triton_requests.clear();
  for (auto& request : sample.requests_) {
    // Capture timestamp before run to avoid incorrect accumulation from
    // sequential warmup runs
#ifdef TRITON_ENABLE_STATS
    request->CaptureRequestStartNs();
#endif  // TRITON_ENABLE_STATS
    request->CaptureQueueStartNs();
    triton_requests.push_back(
        reinterpret_cast<TRITONBACKEND_Request*>(request.release()));
  }
  TRITONBACKEND_ModelInstance* triton_model_instance =
      reinterpret_cast<TRITONBACKEND_ModelInstance*>(
          instances_[runner_idx].get());
  TritonBackend::TritonModelInstanceExecFn_t inst_exec_fn =
      backend_->ModelInstanceExecFn();

  // If there is an error then we retain ownership of 'requests'
  // and must send error responses.
  TRITONSERVER_Error* err = inst_exec_fn(
      triton_model_instance, &triton_requests[0], triton_requests.size());
  if (err != nullptr) {
    Status status = Status(
        TritonCodeToStatusCode(TRITONSERVER_ErrorCode(err)),
        TRITONSERVER_ErrorMessage(err));
    for (TRITONBACKEND_Request* tr : triton_requests) {
      std::unique_ptr<InferenceRequest> ur(
          reinterpret_cast<InferenceRequest*>(tr));
      InferenceRequest::RespondIfError(ur, status, true /* release_requests */);
    }

    TRITONSERVER_ErrorDelete(err);
  }
}

TritonModel::TritonModel(
    InferenceServer* server,
    const std::shared_ptr<LocalizedDirectory>& localized_model_dir,
    const std::shared_ptr<TritonBackend>& backend,
    const double min_compute_capability)
    : InferenceBackend(min_compute_capability), server_(server),
      localized_model_dir_(localized_model_dir), backend_(backend),
      state_(nullptr)
{
}

TritonModel::~TritonModel()
{
  // Explicitly delete/finalize all model instances before finalizing
  // the model itself.
  instances_.clear();

  // Model finalization is optional... The TRITONBACKEND_Model
  // object is this TritonModel object.
  if (backend_->ModelFiniFn() != nullptr) {
    LOG_TRITONSERVER_ERROR(
        backend_->ModelFiniFn()(reinterpret_cast<TRITONBACKEND_Model*>(this)),
        "failed finalizing model");
  }
}

extern "C" {

//
// TRITONBACKEND_Model
//
TRITONSERVER_Error*
TRITONBACKEND_ModelName(TRITONBACKEND_Model* model, const char** name)
{
  TritonModel* tm = reinterpret_cast<TritonModel*>(model);
  *name = tm->Name().c_str();
  return nullptr;  // success
}

TRITONSERVER_Error*
TRITONBACKEND_ModelVersion(TRITONBACKEND_Model* model, uint64_t* version)
{
  TritonModel* tm = reinterpret_cast<TritonModel*>(model);
  *version = tm->Version();
  return nullptr;  // success
}

TRITONSERVER_Error*
TRITONBACKEND_ModelRepository(
    TRITONBACKEND_Model* model, TRITONBACKEND_ModelArtifactType* artifact_type,
    const char** location)
{
  TritonModel* tm = reinterpret_cast<TritonModel*>(model);
  *artifact_type = TRITONBACKEND_ARTIFACT_FILESYSTEM;
  *location = tm->LocalizedModelPath().c_str();
  return nullptr;  // success
}

TRITONSERVER_Error*
TRITONBACKEND_ModelConfig(
    TRITONBACKEND_Model* model, const uint32_t config_version,
    TRITONSERVER_Message** model_config)
{
  TritonModel* tm = reinterpret_cast<TritonModel*>(model);

  std::string model_config_json;
  Status status =
      ModelConfigToJson(tm->Config(), config_version, &model_config_json);
  if (!status.IsOk()) {
    return TRITONSERVER_ErrorNew(
        StatusCodeToTritonCode(status.StatusCode()), status.Message().c_str());
  }

  *model_config = reinterpret_cast<TRITONSERVER_Message*>(
      new TritonServerMessage(std::move(model_config_json)));

  return nullptr;  // success
}

TRITONSERVER_Error*
TRITONBACKEND_ModelServer(
    TRITONBACKEND_Model* model, TRITONSERVER_Server** server)
{
  TritonModel* tm = reinterpret_cast<TritonModel*>(model);
  *server = reinterpret_cast<TRITONSERVER_Server*>(tm->Server());
  return nullptr;  // success
}

TRITONSERVER_Error*
TRITONBACKEND_ModelBackend(
    TRITONBACKEND_Model* model, TRITONBACKEND_Backend** backend)
{
  TritonModel* tm = reinterpret_cast<TritonModel*>(model);
  *backend = reinterpret_cast<TRITONBACKEND_Backend*>(tm->Backend().get());
  return nullptr;  // success
}

TRITONSERVER_Error*
TRITONBACKEND_ModelState(TRITONBACKEND_Model* model, void** state)
{
  TritonModel* tm = reinterpret_cast<TritonModel*>(model);
  *state = tm->State();
  return nullptr;  // success
}

TRITONSERVER_Error*
TRITONBACKEND_ModelSetState(TRITONBACKEND_Model* model, void* state)
{
  TritonModel* tm = reinterpret_cast<TritonModel*>(model);
  tm->SetState(state);
  return nullptr;  // success
}

///
/// TRITONBACKEND_Request
///
TRITONSERVER_Error*
TRITONBACKEND_RequestId(TRITONBACKEND_Request* request, const char** id)
{
  InferenceRequest* tr = reinterpret_cast<InferenceRequest*>(request);
  *id = tr->Id().c_str();
  return nullptr;  // success
}

TRITONSERVER_Error*
TRITONBACKEND_RequestCorrelationId(TRITONBACKEND_Request* request, uint64_t* id)
{
  InferenceRequest* tr = reinterpret_cast<InferenceRequest*>(request);
  *id = tr->CorrelationId();
  return nullptr;  // success
}

TRITONSERVER_Error*
TRITONBACKEND_RequestInputCount(TRITONBACKEND_Request* request, uint32_t* count)
{
  InferenceRequest* tr = reinterpret_cast<InferenceRequest*>(request);
  *count = tr->ImmutableInputs().size();
  return nullptr;  // success
}

TRITONSERVER_Error*
TRITONBACKEND_RequestInputName(
    TRITONBACKEND_Request* request, const uint32_t index,
    const char** input_name)
{
  *input_name = nullptr;

  InferenceRequest* tr = reinterpret_cast<InferenceRequest*>(request);
  const auto& inputs = tr->ImmutableInputs();
  if (index >= inputs.size()) {
    return TRITONSERVER_ErrorNew(
        TRITONSERVER_ERROR_INVALID_ARG,
        (std::string("out of bounds index ") + std::to_string(index) +
         ": request has " + std::to_string(inputs.size()) + " inputs")
            .c_str());
  }

  // The request inputs are not allowed to change once the request
  // makes it to the backend, so it is ok to just iterate through the
  // map. This linear search is the best we can do given the need for
  // the inputs to be in a map and given the typical small number of
  // inputs is better than having every request maintain the inputs as
  // both map and vector.
  uint32_t cnt = 0;
  for (const auto& pr : inputs) {
    if (cnt++ == index) {
      InferenceRequest::Input* in = pr.second;
      *input_name = in->Name().c_str();
      break;
    }
  }

  return nullptr;  // success
}

TRITONSERVER_Error*
TRITONBACKEND_RequestInput(
    TRITONBACKEND_Request* request, const char* name,
    TRITONBACKEND_Input** input)
{
  InferenceRequest* tr = reinterpret_cast<InferenceRequest*>(request);
  const auto& inputs = tr->ImmutableInputs();
  const auto& itr = inputs.find(name);
  if (itr == inputs.end()) {
    *input = nullptr;
    return TRITONSERVER_ErrorNew(
        TRITONSERVER_ERROR_INVALID_ARG,
        (std::string("unknown request input name ") + name).c_str());
  }

  InferenceRequest::Input* in = itr->second;
  *input = reinterpret_cast<TRITONBACKEND_Input*>(in);

  return nullptr;  // success
}

TRITONSERVER_Error*
TRITONBACKEND_RequestOutputCount(
    TRITONBACKEND_Request* request, uint32_t* count)
{
  InferenceRequest* tr = reinterpret_cast<InferenceRequest*>(request);
  *count = tr->ImmutableRequestedOutputs().size();
  return nullptr;  // success
}

TRITONSERVER_Error*
TRITONBACKEND_RequestOutputName(
    TRITONBACKEND_Request* request, const uint32_t index,
    const char** output_name)
{
  *output_name = nullptr;

  InferenceRequest* tr = reinterpret_cast<InferenceRequest*>(request);
  const auto& routputs = tr->ImmutableRequestedOutputs();
  if (index >= routputs.size()) {
    return TRITONSERVER_ErrorNew(
        TRITONSERVER_ERROR_INVALID_ARG,
        (std::string("out of bounds index ") + std::to_string(index) +
         ": request has " + std::to_string(routputs.size()) +
         " requested outputs")
            .c_str());
  }

  // The requested outputs are not allowed to change once the request
  // makes it to the backend, so it is ok to just iterate through the
  // set. This linear search is the best we can do given the requested
  // outputs being in a set and given the typical small number of
  // requested outputs it should not be a performance issue.
  uint32_t cnt = 0;
  for (const auto& rout : routputs) {
    if (cnt++ == index) {
      *output_name = rout.c_str();
      break;
    }
  }

  return nullptr;  // success
}

TRITONSERVER_Error*
TRITONBACKEND_RequestRelease(
    TRITONBACKEND_Request* request, uint32_t release_flags)
{
  InferenceRequest* tr = reinterpret_cast<InferenceRequest*>(request);
  std::unique_ptr<InferenceRequest> ur(tr);
  InferenceRequest::Release(std::move(ur), release_flags);
  return nullptr;  // success
}

//
// TRITONBACKEND_ResponseFactory
//
TRITONSERVER_Error*
TRITONBACKEND_ResponseFactoryNew(
    TRITONBACKEND_ResponseFactory** factory, TRITONBACKEND_Request* request)
{
  InferenceRequest* tr = reinterpret_cast<InferenceRequest*>(request);
  InferenceResponseFactory* response_factory =
      new InferenceResponseFactory(tr->ResponseFactory());
  *factory = reinterpret_cast<TRITONBACKEND_ResponseFactory*>(response_factory);
  return nullptr;  // success
}

TRITONSERVER_Error*
TRITONBACKEND_ResponseFactoryDelete(TRITONBACKEND_ResponseFactory* factory)
{
  InferenceResponseFactory* tf =
      reinterpret_cast<InferenceResponseFactory*>(factory);
  delete tf;
  return nullptr;  // success
}

TRITONSERVER_Error*
TRITONBACKEND_ResponseFactorySendFlags(
    TRITONBACKEND_ResponseFactory* factory, const uint32_t send_flags)
{
  InferenceResponseFactory* tf =
      reinterpret_cast<InferenceResponseFactory*>(factory);
  Status status = tf->SendFlags(send_flags);
  if (!status.IsOk()) {
    return TRITONSERVER_ErrorNew(
        StatusCodeToTritonCode(status.StatusCode()), status.Message().c_str());
  }
  return nullptr;  // success
}

///
/// TRITONBACKEND_Response
///
TRITONSERVER_Error*
TRITONBACKEND_ResponseNew(
    TRITONBACKEND_Response** response, TRITONBACKEND_Request* request)
{
  InferenceRequest* tr = reinterpret_cast<InferenceRequest*>(request);

  std::unique_ptr<InferenceResponse> tresp;
  Status status = tr->ResponseFactory().CreateResponse(&tresp);
  if (!status.IsOk()) {
    *response = nullptr;
    return TRITONSERVER_ErrorNew(
        StatusCodeToTritonCode(status.StatusCode()), status.Message().c_str());
  }

  *response = reinterpret_cast<TRITONBACKEND_Response*>(tresp.release());
  return nullptr;  // success
}

TRITONSERVER_Error*
TRITONBACKEND_ResponseNewFromFactory(
    TRITONBACKEND_Response** response, TRITONBACKEND_ResponseFactory* factory)
{
  InferenceResponseFactory* tf =
      reinterpret_cast<InferenceResponseFactory*>(factory);

  std::unique_ptr<InferenceResponse> tr;
  Status status = tf->CreateResponse(&tr);
  if (!status.IsOk()) {
    *response = nullptr;
    return TRITONSERVER_ErrorNew(
        StatusCodeToTritonCode(status.StatusCode()), status.Message().c_str());
  }

  *response = reinterpret_cast<TRITONBACKEND_Response*>(tr.release());
  return nullptr;  // success
}

TRITONSERVER_Error*
TRITONBACKEND_ResponseDelete(TRITONBACKEND_Response* response)
{
  InferenceResponse* tr = reinterpret_cast<InferenceResponse*>(response);
  delete tr;
  return nullptr;  // success
}

TRITONSERVER_Error*
TRITONBACKEND_ResponseSetStringParameter(
    TRITONBACKEND_Response* response, const char* name, const char* value)
{
  InferenceResponse* tr = reinterpret_cast<InferenceResponse*>(response);
  Status status = tr->AddParameter(name, value);
  if (!status.IsOk()) {
    return TRITONSERVER_ErrorNew(
        StatusCodeToTritonCode(status.StatusCode()), status.Message().c_str());
  }
  return nullptr;  // success
}

TRITONSERVER_Error*
TRITONBACKEND_ResponseSetIntParameter(
    TRITONBACKEND_Response* response, const char* name, const int64_t value)
{
  InferenceResponse* tr = reinterpret_cast<InferenceResponse*>(response);
  Status status = tr->AddParameter(name, value);
  if (!status.IsOk()) {
    return TRITONSERVER_ErrorNew(
        StatusCodeToTritonCode(status.StatusCode()), status.Message().c_str());
  }
  return nullptr;  // success
}

TRITONSERVER_Error*
TRITONBACKEND_ResponseSetBoolParameter(
    TRITONBACKEND_Response* response, const char* name, const bool value)
{
  InferenceResponse* tr = reinterpret_cast<InferenceResponse*>(response);
  Status status = tr->AddParameter(name, value);
  if (!status.IsOk()) {
    return TRITONSERVER_ErrorNew(
        StatusCodeToTritonCode(status.StatusCode()), status.Message().c_str());
  }
  return nullptr;  // success
}

TRITONSERVER_Error*
TRITONBACKEND_ResponseOutput(
    TRITONBACKEND_Response* response, TRITONBACKEND_Output** output,
    const char* name, const TRITONSERVER_DataType datatype,
    const int64_t* shape, const uint32_t dims_count)
{
  InferenceResponse* tr = reinterpret_cast<InferenceResponse*>(response);
  std::vector<int64_t> lshape(shape, shape + dims_count);
  InferenceResponse::Output* loutput;
  Status status = tr->AddOutput(
      name, TritonToDataType(datatype), std::move(lshape), &loutput);
  if (!status.IsOk()) {
    *output = nullptr;
    return TRITONSERVER_ErrorNew(
        StatusCodeToTritonCode(status.StatusCode()), status.Message().c_str());
  }

  *output = reinterpret_cast<TRITONBACKEND_Output*>(loutput);
  return nullptr;  // success
}

TRITONSERVER_Error*
TRITONBACKEND_ResponseSend(
    TRITONBACKEND_Response* response, const uint32_t send_flags,
    TRITONSERVER_Error* error)
{
  InferenceResponse* tr = reinterpret_cast<InferenceResponse*>(response);

  Status status;

  std::unique_ptr<InferenceResponse> utr(tr);
  if (error == nullptr) {
    status = InferenceResponse::Send(std::move(utr), send_flags);
  } else {
    status = InferenceResponse::SendWithStatus(
        std::move(utr), send_flags,
        Status(
            TritonCodeToStatusCode(TRITONSERVER_ErrorCode(error)),
            TRITONSERVER_ErrorMessage(error)));
  }

  if (!status.IsOk()) {
    return TRITONSERVER_ErrorNew(
        StatusCodeToTritonCode(status.StatusCode()), status.Message().c_str());
  }

  return nullptr;  // success
}

///
/// TRITONBACKEND_Input
///
TRITONSERVER_Error*
TRITONBACKEND_InputProperties(
    TRITONBACKEND_Input* input, const char** name,
    TRITONSERVER_DataType* datatype, const int64_t** shape,
    uint32_t* dims_count, uint64_t* byte_size, uint32_t* buffer_count)
{
  InferenceRequest::Input* ti =
      reinterpret_cast<InferenceRequest::Input*>(input);
  if (name != nullptr) {
    *name = ti->Name().c_str();
  }
  if (datatype != nullptr) {
    *datatype = DataTypeToTriton(ti->DType());
  }
  if (shape != nullptr) {
    *shape = ti->ShapeWithBatchDim().data();
  }
  if (dims_count != nullptr) {
    *dims_count = ti->ShapeWithBatchDim().size();
  }
  if (byte_size != nullptr) {
    *byte_size = GetByteSize(ti->DType(), ti->ShapeWithBatchDim());
  }
  if (buffer_count != nullptr) {
    *buffer_count = ti->DataBufferCount();
  }
  return nullptr;  // success
}

TRITONSERVER_Error*
TRITONBACKEND_InputBuffer(
    TRITONBACKEND_Input* input, const uint32_t index, const void** buffer,
    uint64_t* buffer_byte_size, TRITONSERVER_MemoryType* memory_type,
    int64_t* memory_type_id)
{
  InferenceRequest::Input* ti =
      reinterpret_cast<InferenceRequest::Input*>(input);
  Status status = ti->DataBuffer(
      index, buffer, buffer_byte_size, memory_type, memory_type_id);
  if (!status.IsOk()) {
    *buffer = nullptr;
    *buffer_byte_size = 0;
    return TRITONSERVER_ErrorNew(
        StatusCodeToTritonCode(status.StatusCode()), status.Message().c_str());
  }
  return nullptr;  // success
}

///
/// TRITONBACKEND_Output
///
TRITONSERVER_Error*
TRITONBACKEND_OutputBuffer(
    TRITONBACKEND_Output* output, void** buffer,
    const uint64_t buffer_byte_size, TRITONSERVER_MemoryType* memory_type,
    int64_t* memory_type_id)
{
  InferenceResponse::Output* to =
      reinterpret_cast<InferenceResponse::Output*>(output);
  Status status = to->AllocateDataBuffer(
      buffer, buffer_byte_size, memory_type, memory_type_id);
  if (!status.IsOk()) {
    *buffer = nullptr;
    return TRITONSERVER_ErrorNew(
        StatusCodeToTritonCode(status.StatusCode()), status.Message().c_str());
  }
  return nullptr;  // success
}

}  // extern C

}}  // namespace nvidia::inferenceserver
