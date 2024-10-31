
#include "QNNRoPE.hpp"
#include "Types.hpp"
#include "QNNCommonOp.hpp"
#include <cstdint>

namespace mllm {

vector<vector<float>> QNNRoPE::sin_;
vector<vector<float>> QNNRoPE::cos_;
int QNNRoPE::global_pose_type_ = -1;
int QNNRoPE::ishape_old;


extern void sinusoidal_position_embedding_llama(int seq_len, int output_dim, vector<vector<float>> &sin, vector<vector<float>> &cos);
extern void sinusoidal_position_embedding_huggingface(int seq_len, int output_dim, vector<vector<float>> &sin, vector<vector<float>> &cos, int base = 10000);

QNNRoPE::QNNRoPE(Backend *bn, string opName, int pose_type) :
    QNNCommonOp(bn, opName) {
    pose_type_ = pose_type;

    sinTensor_.setBackend(bn);
    cosTensor_.setBackend(bn);
    hcntTensor_.setBackend(bn);
}

QNNRoPE::QNNRoPE(Backend *bn, string opName, int pose_type, float rope_theta, int max_position_embeddings) :
    QNNCommonOp(bn, opName) {
    pose_type_ = pose_type;
    rope_theta_ = rope_theta;
    pos_max_ = max_position_embeddings;

    sinTensor_.setBackend(bn);
    cosTensor_.setBackend(bn);
    hcntTensor_.setBackend(bn);
}

QNNRoPE::QNNRoPE(Backend *bn, string opName, int pose_type, float rope_theta, float partial_rotary_factor, int max_position_embeddings) :
    QNNCommonOp(bn, opName) {
    pose_type_ = pose_type;
    rope_theta_ = rope_theta;
    partial_rotary_factor_ = partial_rotary_factor;
    pos_max_ = max_position_embeddings;

    sinTensor_.setBackend(bn);
    cosTensor_.setBackend(bn);
    hcntTensor_.setBackend(bn);
}

ErrorCode QNNRoPE::reshape(vector<shared_ptr<Tensor>> inputs, vector<shared_ptr<Tensor>> outputs) {
    assert(inputs.size() == 1);
    assert(outputs.size() == 1);
    outputs[0]->reshape(inputs[0]->batch(), inputs[0]->head(), inputs[0]->sequence(), inputs[0]->dimension());

    ishape = inputs[0]->dimension() * partial_rotary_factor_;

    return Op::reshape(inputs, outputs);
}

ErrorCode QNNRoPE::setUp(vector<shared_ptr<Tensor>> inputs, vector<shared_ptr<Tensor>> outputs) {
    // in case ishape is 0 when Op is the first one in the graph

    if (sin_.empty() || ishape_old < ishape || global_pose_type_ != pose_type_) {
        global_pose_type_ = pose_type_;
        ishape_old = ishape;
        if (pose_type_ == LLAMAROPE) {
            sinusoidal_position_embedding_llama(pos_max_, ishape, sin_, cos_);
        } else if (pose_type_ == PERSIMMONROPE) {
            sinusoidal_position_embedding_huggingface(pos_max_, ishape / 2, sin_, cos_, 25000);
        } else if (pose_type_ == HFHUBROPE || pose_type_ == MLAROPE) {
            sinusoidal_position_embedding_huggingface(pos_max_, ishape, sin_, cos_, rope_theta_);
        } else {
        }
    }

#ifdef OLD_QNN
    if (getOutputTensorType(outputs[0]) == QNN_TENSOR_TYPE_APP_READ) {
        outputs[0]->setBackend(qnnBackend_);
        outputs[0]->setDtype(MLLM_TYPE_F32);
        outputs[0]->alloc();

        qnnBackend_->pushOutputBuffers(outputs[0]->hostPtr<uint8_t>());
    }
#endif

    auto type = QNN_DATATYPE_FLOAT_32;
    if (inputs[0]->dtype() == MLLM_TYPE_F32) {

        sinTensor_.setName(name() + ".sin");
        sinTensor_.reshape(1, 1, pos_max_, ishape);
        sinTensor_.setDtype(MLLM_TYPE_F32);
        sinTensor_.alloc();


        cosTensor_.setName(name() + ".cos");
        cosTensor_.reshape(1, 1, pos_max_, ishape);
        cosTensor_.setDtype(MLLM_TYPE_F32);
        cosTensor_.alloc();

        for (int i = 0; i<pos_max_; i++) {
            for (int j=0; j<ishape; j++) {
                sinTensor_.setDataAt<float>(0, 0, i, j, sin_[i][j]);
                cosTensor_.setDataAt<float>(0, 0, i, j, cos_[i][j]);
            }
        }
        
    }  else if (inputs[0]->dtype() == MLLM_TYPE_F16) {
        
        sinTensor_.setName(name() + ".sin");
        sinTensor_.reshape(1, 1, pos_max_, ishape);
        sinTensor_.setDtype(MLLM_TYPE_F32);
        sinTensor_.alloc();


        cosTensor_.setName(name() + ".cos");
        cosTensor_.reshape(1, 1, pos_max_, ishape);
        cosTensor_.setDtype(MLLM_TYPE_F32);
        cosTensor_.alloc();

        for (int i = 0; i<pos_max_; i++) {
            for (int j=0; j<ishape; j++) {
                sinTensor_.setDataAt<float>(0, 0, i, j, static_cast<float>(sin_[i][j]));
                cosTensor_.setDataAt<float>(0, 0, i, j, static_cast<float>(cos_[i][j]));
            }
        }

        type = QNN_DATATYPE_FLOAT_16;

    } 
    



    


    uint32_t sin_dimensions[] = {static_cast<uint32_t>(pos_max_), static_cast<uint32_t>(ishape)};
    uint32_t cos_dimensions[] = {static_cast<uint32_t>(pos_max_), static_cast<uint32_t>(ishape)};

    auto sinWeightsName = name() + ".sin.weights";

    qnnBackend_->modelAddTensor(sinWeightsName, // Node Name
                                (Qnn_Tensor_t){
                                    .version = QNN_TENSOR_VERSION_1,
                                    .v1 = {
                                        .id = 0,
                                        .name = sinWeightsName.c_str(),
                                        .type = QNN_TENSOR_TYPE_STATIC,
                                        .dataFormat = QNN_TENSOR_DATA_FORMAT_FLAT_BUFFER,
                                        .dataType = QNN_DATATYPE_FLOAT_32,
                                        .quantizeParams = {QNN_DEFINITION_UNDEFINED,
                                                           QNN_QUANTIZATION_ENCODING_UNDEFINED,
                                                           {.scaleOffsetEncoding = {.scale = 0.0000000000000000f, .offset = 0}}},
                                        .rank = 2,
                                        .dimensions = sin_dimensions,
                                        .memType = QNN_TENSORMEMTYPE_RAW,
                                        .clientBuf = {.data = sinTensor_.hostPtr<uint8_t>(),
                                                      .dataSize = static_cast<uint32_t>(sinTensor_.cntSize())}}});
    // free sin_
    // sin_.free();

    auto cosWeightsName = name() + ".cos.weights";
    qnnBackend_->modelAddTensor(cosWeightsName, // Node Name
                                (Qnn_Tensor_t){
                                    .version = QNN_TENSOR_VERSION_1,
                                    .v1 = {

                                        .id = 0,
                                        .name = cosWeightsName.c_str(),
                                        .type = QNN_TENSOR_TYPE_STATIC,
                                        .dataFormat = QNN_TENSOR_DATA_FORMAT_FLAT_BUFFER,
                                        .dataType = QNN_DATATYPE_FLOAT_32,
                                        .quantizeParams = {QNN_DEFINITION_UNDEFINED,
                                                           QNN_QUANTIZATION_ENCODING_UNDEFINED,
                                                           {.scaleOffsetEncoding = {.scale = 0.0000000000000000f, .offset = 0}}},
                                        .rank = 2,
                                        .dimensions = cos_dimensions,
                                        .memType = QNN_TENSORMEMTYPE_RAW,
                                        .clientBuf = {.data = cosTensor_.hostPtr<uint8_t>(),
                                                      .dataSize = static_cast<uint32_t>(cosTensor_.cntSize())}}});
    // free cos_
    // cos_.free();

    auto hcntName = name() + ".h_cnt";
    uint32_t hcntDimension[1] = {1};
    qnnBackend_->modelAddTensor(hcntName, // Node Name
                                (Qnn_Tensor_t){
                                    .version = QNN_TENSOR_VERSION_1,
                                    .v1 = {
                                        .id = 0,
                                        .name = hcntName.c_str(),
                                        .type = QNN_TENSOR_TYPE_APP_WRITE,
                                        .dataFormat = QNN_TENSOR_DATA_FORMAT_FLAT_BUFFER,
                                        .dataType = QNN_DATATYPE_UINT_32,
                                        .quantizeParams = {QNN_DEFINITION_UNDEFINED,
                                                           QNN_QUANTIZATION_ENCODING_UNDEFINED,
                                                           {.scaleOffsetEncoding = {.scale = 0.0000000000000000f, .offset = 0}}},
                                        .rank = 1,
                                        .dimensions = hcntDimension,
                                        .memType = QNN_TENSORMEMTYPE_RAW,
                                        .clientBuf = {.data = nullptr,
                                                      .dataSize = 0}}});

    qnnBackend_->pushInputBuffers(hcntTensor_.hostPtr<uint8_t>());

    vector<Qnn_Param_t> params_rope = {
        {.paramType = QNN_PARAMTYPE_SCALAR,
         .name = "pose_type",
         .scalarParam = (Qnn_Scalar_t){QNN_DATATYPE_UINT_32, {.uint32Value = static_cast<uint32_t>(global_pose_type_)}}}};

    uint32_t dimOut[4] = {static_cast<uint32_t>(inputs[0]->batch()),
                          static_cast<uint32_t>(inputs[0]->sequence()),
                          static_cast<uint32_t>(inputs[0]->head()),
                          static_cast<uint32_t>(inputs[0]->dimension())};

    auto outName = outputs[0]->name();
    vector<Qnn_Tensor_t> out = {
        (Qnn_Tensor_t){
            .version = QNN_TENSOR_VERSION_1,
            .v1 = {
                .id = 0,
                .name = outName.c_str(),
                .type = getOutputTensorType(outputs[0]),
                .dataFormat = QNN_TENSOR_DATA_FORMAT_FLAT_BUFFER,
                .dataType = type,
                .quantizeParams = {QNN_DEFINITION_UNDEFINED,
                                   QNN_QUANTIZATION_ENCODING_UNDEFINED,
                                   {.scaleOffsetEncoding = {.scale = 0.0000000000000000f, .offset = 0}}},
                .rank = 4,
                .dimensions = dimOut,
                .memType = QNN_TENSORMEMTYPE_RAW,
                .clientBuf = {.data = nullptr,
                              .dataSize = 0}}}};

    return graphAddNode(name(), "RoPE", {inputs[0]->name(), sinWeightsName, cosWeightsName, hcntName}, out, params_rope, "LLaMAPackage");
}

ErrorCode QNNRoPE::load(AbstructLoader &loader) {

    hcntTensor_.setName(name() + ".hcnt.tensor");
    hcntTensor_.reshape(1, 1, 1, 1);
    hcntTensor_.setDtype(MLLM_TYPE_I32);
    hcntTensor_.alloc();

    return Op::load(loader);
}

ErrorCode QNNRoPE::free(vector<shared_ptr<Tensor>> inputs, vector<shared_ptr<Tensor>> outputs) {
    return Op::free(inputs, outputs);
}

ErrorCode QNNRoPE::execute(vector<shared_ptr<Tensor>> inputs, vector<shared_ptr<Tensor>> outputs) {

    h_cnt_ += inputs[0]->sequence();
    hcntTensor_.setDataAt(0,0,0,0, h_cnt_);

    return QNNCommonOp::execute(inputs, outputs);
}

} // namespace mllm
