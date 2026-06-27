#ifndef ONNXRUNTIME_C_API_H
#define ONNXRUNTIME_C_API_H

#include <stdint.h>
#include <stddef.h>

// ============================================================
// Minimal ONNX Runtime C API 类型定义
// 用于 Qwen2.5-0.5B 本地推理
// 完整版参考：https://github.com/microsoft/onnxruntime
// ============================================================

#ifndef ORT_API_CALL
#define ORT_API_CALL
#endif

#ifdef __cplusplus
extern "C" {
#endif

// ---------- 枚举 ----------

typedef enum OrtLoggingLevel_ {
    ORT_LOGGING_LEVEL_VERBOSE,
    ORT_LOGGING_LEVEL_INFO,
    ORT_LOGGING_LEVEL_WARNING,
    ORT_LOGGING_LEVEL_ERROR,
    ORT_LOGGING_LEVEL_FATAL,
} OrtLoggingLevel;

typedef enum OrtAllocatorType_ {
    OrtInvalidAllocator = -1,
    OrtDeviceAllocator = 0,
    OrtArenaAllocator = 1
} OrtAllocatorType;

typedef enum OrtMemType_ {
    OrtMemTypeCPUInput = -2,
    OrtMemTypeCPUOutput = -1,
    OrtMemTypeCPU = 0,
    OrtMemTypeGPU = 1,
} OrtMemType;

typedef enum OrtValueType_ {
    ORT_TYPE_UNKNOWN = 0,
    ORT_TYPE_TENSOR = 1,
    ORT_TYPE_SEQUENCE = 2,
    ORT_TYPE_MAP = 3,
} OrtValueType;

typedef enum OrtTensorType_ {
    ORT_TENSOR_UNDEFINED = 0,
    ORT_TENSOR_FLOAT = 1,
    ORT_TENSOR_UINT8 = 2,
    ORT_TENSOR_INT8 = 3,
    ORT_TENSOR_UINT16 = 4,
    ORT_TENSOR_INT16 = 5,
    ORT_TENSOR_INT32 = 6,
    ORT_TENSOR_INT64 = 7,
    ORT_TENSOR_STRING = 8,
    ORT_TENSOR_BOOL = 9,
    ORT_TENSOR_FLOAT16 = 10,
    ORT_TENSOR_DOUBLE = 11,
    ORT_TENSOR_UINT32 = 12,
    ORT_TENSOR_UINT64 = 13,
    ORT_TENSOR_COMPLEX64 = 14,
    ORT_TENSOR_COMPLEX128 = 15,
    ORT_TENSOR_BFLOAT16 = 16,
} OrtTensorType;

typedef enum OrtErrorCode_ {
    ORT_OK,
    ORT_FAIL,
    ORT_INVALID_ARGUMENT,
    ORT_NO_SUCHFILE,
    ORT_NO_MODEL,
    ORT_ENGINE_ERROR,
    ORT_RUNTIME_EXCEPTION,
    ORT_INVALID_PROTOBUF,
    ORT_MODEL_LOADED,
    ORT_NOT_IMPLEMENTED,
    ORT_INVALID_GRAPH,
    ORT_EP_FAIL,
} OrtErrorCode;

// ---------- 不透明句柄 ----------

typedef struct OrtEnv OrtEnv;
typedef struct OrtSession OrtSession;
typedef struct OrtSessionOptions OrtSessionOptions;
typedef struct OrtValue OrtValue;
typedef struct OrtMemoryInfo OrtMemoryInfo;
typedef struct OrtRunOptions OrtRunOptions;
typedef struct OrtAllocator OrtAllocator;
typedef struct OrtIoBinding OrtIoBinding;
typedef struct OrtTensorTypeAndShapeInfo OrtTensorTypeAndShapeInfo;
typedef struct OrtTypeInfo OrtTypeInfo;
typedef struct OrtApi OrtApi;
typedef struct OrtStatus OrtStatus;
typedef struct OrtModelMetadata OrtModelMetadata;

// ---------- API 表 ----------

struct OrtApi {
    // 基础
    OrtStatus*(ORT_API_CALL* CreateEnv)(OrtLoggingLevel logging_level, const char* logid, OrtEnv** out);
    OrtStatus*(ORT_API_CALL* ReleaseEnv)(OrtEnv*);
    OrtStatus*(ORT_API_CALL* CreateSessionOptions)(OrtSessionOptions** out);
    OrtStatus*(ORT_API_CALL* SetSessionThreadPoolSize)(OrtSessionOptions*, int);
    OrtStatus*(ORT_API_CALL* SetSessionGraphOptimizationLevel)(OrtSessionOptions*, int);
    OrtStatus*(ORT_API_CALL* ReleaseSessionOptions)(OrtSessionOptions*);
    OrtStatus*(ORT_API_CALL* SetIntraOpNumThreads)(OrtSessionOptions*, int);
    OrtStatus*(ORT_API_CALL* SetInterOpNumThreads)(OrtSessionOptions*, int);
    
    // Session
    OrtStatus*(ORT_API_CALL* CreateSession)(OrtEnv*, const char* model_path, OrtSessionOptions*, OrtSession** out);
    OrtStatus*(ORT_API_CALL* Run)(OrtSession*, OrtRunOptions*, const char* const* input_names, const OrtValue* const* input, size_t input_len, const char* const* output_names, size_t output_len, OrtValue** output);
    OrtStatus*(ORT_API_CALL* ReleaseSession)(OrtSession*);
    
    // Tensor
    OrtStatus*(ORT_API_CALL* CreateTensorWithDataAsOrtValue)(const OrtMemoryInfo* info, void* p_data, size_t data_len, const int64_t* shape, size_t shape_len, OrtTensorType type, OrtValue** out);
    OrtStatus*(ORT_API_CALL* CreateTensorAsOrtValue)(OrtAllocator* allocator, const int64_t* shape, size_t shape_len, OrtTensorType type, OrtValue** out);
    OrtStatus*(ORT_API_CALL* GetTensorMutableData)(OrtValue*, void** out);
    OrtStatus*(ORT_API_CALL* GetTensorTypeAndShape)(const OrtValue*, OrtTensorTypeAndShapeInfo** out);
    OrtStatus*(ORT_API_CALL* GetTensorShapeElementCount)(const OrtTensorTypeAndShapeInfo*, size_t* out);
    OrtStatus*(ORT_API_CALL* GetTensorTypeAndShapeInfo_GetShape)(const OrtTensorTypeAndShapeInfo*, int64_t* out, size_t dim_count, size_t* out_dim_count);
    OrtStatus*(ORT_API_CALL* GetTensorTypeAndShapeInfo_GetDimensionsCount)(const OrtTensorTypeAndShapeInfo*, size_t* out);
    OrtStatus*(ORT_API_CALL* ReleaseTensorTypeAndShapeInfo)(OrtTensorTypeAndShapeInfo*);
    OrtStatus*(ORT_API_CALL* ReleaseValue)(OrtValue*);
    
    // Memory
    OrtStatus*(ORT_API_CALL* CreateCpuMemoryInfo)(OrtAllocatorType, OrtMemType, OrtMemoryInfo** out);
    OrtStatus*(ORT_API_CALL* ReleaseMemoryInfo)(OrtMemoryInfo*);
    
    // Allocator
    OrtStatus*(ORT_API_CALL* GetAllocatorWithDefaultOptions)(OrtAllocator** out);
    
    // Run options
    OrtStatus*(ORT_API_CALL* CreateRunOptions)(OrtRunOptions** out);
    OrtStatus*(ORT_API_CALL* ReleaseRunOptions)(OrtRunOptions*);
    
    // Metadata
    OrtStatus*(ORT_API_CALL* SessionGetInputCount)(OrtSession*, size_t* out);
    OrtStatus*(ORT_API_CALL* SessionGetOutputCount)(OrtSession*, size_t* out);
    OrtStatus*(ORT_API_CALL* SessionGetInputName)(OrtSession*, size_t index, OrtAllocator*, char** out);
    OrtStatus*(ORT_API_CALL* SessionGetOutputName)(OrtSession*, size_t index, OrtAllocator*, char** out);
    OrtStatus*(ORT_API_CALL* Free)(void*);
};

// ---------- 获取 API 表 ----------
// 编译时不需要此函数声明，OrtGetApiBase 通过 dlsym 动态获取
#define OrtGetApiBase OrtGetApiBase

#ifdef __cplusplus
}
#endif

#endif // ONNXRUNTIME_C_API_H
