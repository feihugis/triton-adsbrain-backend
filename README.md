# Triton Adsbrain Backend

The triton backend for adsbrain users to run their C++ based models on triton
server. It assumes that 1) triton-adsbrain server is used with `AB_IN_OUT_RAW=1`
and `AB_REQUEST_TYPE=ADSBRAIN_BOND`; 2) the model takes one string input and the
input name in config.pbtxt is assumed to be `raw_input` in the shape of [1]; 3) 
the model only returns one string output in the shape of [1] and the output name
in config.pbtxt will not be used; 4) the input and output data will be in the 
format of `raw_format`, which means no metadata info required for input or output.

## How to build
```
cmake \
  -DCMAKE_INSTALL_PREFIX:PATH=`pwd`/install \
  -DTRITON_COMMON_REPO_TAG:STRING=r22.05 \
  -DTRITON_CORE_REPO_TAG:STRING=r22.05 \
  -DTRITON_BACKEND_REPO_TAG:STRING=r22.05 \
  -DCMAKE_BUILD_TYPE:STRING=Release \
  ..

make -j install
```

## How to implement the model for the adsbrain backend

1) Compile the adsbrain backend and copy `adsbrain_backend.h` and 
   `libtriton_backend.so` to your project;
2) Derive `class AdsbrainInferenceModel` to implement the model-specific logic;
3) Implement the C API `CreateInferenceModel(...)` to create the model instance;
4) Compile the C++ model inference code into a shared library and put it and all
   the dependent shared libraies to the model serving directory;
5) Update `config.pbtxt` to use the adsbrain backend and specify the shared
   library name and required parameters.