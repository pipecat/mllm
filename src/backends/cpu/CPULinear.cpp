
#include "CPULinear.hpp"

namespace mllm {

CPULinear::CPULinear(Backend *bn, int in_features, int out_features, bool bias, bool multiThread) :
    Op(bn) {
    in_features_ = in_features;
    out_features_ = out_features;
    support_bias_ = bias;
    support_multi_thread_ = multiThread;
    weight_.setBackend(bn);
    bias_.setBackend(bn);
}

ErrorCode CPULinear::reshape(vector<shared_ptr<Tensor>> &inputs, vector<shared_ptr<Tensor>> &outputs) {
    std::cout << "CPULinear  reshape" << std::endl;
    CHECK_EQ(inputs.size(), 1);
    CHECK_EQ(outputs.size(), 1);
    // N     |    C       |   H                   |  W
    // -----------------------------------------------
    // 1     |out_channel | in_channel            |  1
    //       |out_features| in_features           |
    // -----------------------------------------------
    // batch |in_channel  | seq_len               |  1
    //       |in_features | inputs[0]->height()   |
    // -----------------------------------------------
    // batch |out_channel | seq_len               |  1
    //       |out_features|  inputs[0]->height()  |
    CHECK_EQ(inputs[0]->width(), 1);
    CHECK_EQ(in_features_, inputs[0]->channels());
    weight_.reshape(1, out_features_, in_features_, 1);
    weight_.setName(name() + ".weight");
    bias_.reshape(1, out_features_, 1, 1);
    bias_.setName(name() + ".bias");
    outputs[0]->reshape(inputs[0]->num(), out_features_, inputs[0]->height(), inputs[0]->width());
    return NO_ERROR;
}

ErrorCode CPULinear::setUp(vector<shared_ptr<Tensor>> &inputs, vector<shared_ptr<Tensor>> &outputs) {
    std::cout << "CPULinear  setUp" << std::endl;
    if (!inputs[0]->allocted()) {
        inputs[0]->alloc(); // TODO remove
    }
    outputs[0]->alloc();
    weight_.alloc();
    bias_.alloc();
    return NO_ERROR;
}

ErrorCode CPULinear::execute(vector<shared_ptr<Tensor>> &inputs, vector<shared_ptr<Tensor>> &outputs) {
    std::cout << "CPULinear()" << std::endl;
    // INPUT: M.K
    // W:K,N
    // OUTPUT:M.N
    int M = out_features_;
    int K = in_features_;
    int N = inputs[0]->height();
    for (int b = 0; b < inputs[0]->num(); b++) {
        for (int w = 0; w < inputs[0]->width(); w++) {
            for (int m = 0; m < M; m++) {
                for (int n = 0; n < N; n++) {
                    float value = 0;
                    for (int k = 0; k < K; k++) {
                        value += weight_.dataAt<float>(1, m, k, w) * inputs[0]->dataAt<float>(b, k, n, w);
                    }
                    if (support_bias_)
                        value += bias_.dataAt<float>(1, m, 1, w);
                    outputs[0]->setDataAt<float>(b, m, n, w, value);
                }
            }
        }
    }

#ifdef DEBUG
    inputs[0]->printData<float>();
    weight_.printData<float>();
    bias_.printData<float>();
    outputs[0]->printData<float>();
#endif

    return NO_ERROR;
}

ErrorCode CPULinear::load(ParamLoader &loader) {
    std::cout << "CPULinear load" << std::endl;
    loader.load(&weight_);
    if (support_bias_)
        loader.load(&bias_);
    return NO_ERROR;
}

} // namespace mllm
