//
// Created by whl on 2025/1/9.
//

#include "libocr.h"

static bool eval_tokens(struct llama_context * ctx_llama, std::vector<llama_token> tokens, int n_batch, int * n_past) {
    int N = (int) tokens.size();
    for (int i = 0; i < N; i += n_batch) {
        int n_eval = (int) tokens.size() - i;
        if (n_eval > n_batch) {
            n_eval = n_batch;
        }
        if (llama_decode(ctx_llama, llama_batch_get_one(&tokens[i], n_eval))) {
            LOG_ERR("%s : failed to eval. token %d/%d (batch size %d, n_past %d)\n", __func__, i, N, n_batch, *n_past);
            return false;
        }
        *n_past += n_eval;
    }
    return true;
}

static bool eval_id(struct llama_context * ctx_llama, int id, int * n_past) {
    std::vector<llama_token> tokens;
    tokens.push_back(id);
    return eval_tokens(ctx_llama, tokens, 1, n_past);
}

static bool eval_string(struct llama_context * ctx_llama, const char * str, int n_batch, int * n_past, bool add_bos) {
    std::string              str2     = str;
    std::vector<llama_token> embd_inp = common_tokenize(ctx_llama, str2, add_bos, true);
    eval_tokens(ctx_llama, embd_inp, n_batch, n_past);
    return true;
}

static const char * sample(struct common_sampler * smpl, struct llama_context * ctx_llama, int * n_past) {
    const llama_token id = common_sampler_sample(smpl, ctx_llama, -1);
    common_sampler_accept(smpl, id, true);
    static std::string ret;
    if (llama_token_is_eog(llama_get_model(ctx_llama), id)) {
        ret = "</s>";
    } else {
        ret = common_token_to_piece(ctx_llama, id);
    }
    eval_id(ctx_llama, id, n_past);
    return ret.c_str();
}

static const char * IMG_BASE64_TAG_BEGIN = "<img src=\"data:image/jpeg;base64,";
static const char * IMG_BASE64_TAG_END   = "\">";

static void find_image_tag_in_prompt(const std::string & prompt, size_t & begin_out, size_t & end_out) {
    begin_out = prompt.find(IMG_BASE64_TAG_BEGIN);
    end_out   = prompt.find(IMG_BASE64_TAG_END, (begin_out == std::string::npos) ? 0UL : begin_out);
}

static bool prompt_contains_image(const std::string & prompt) {
    size_t begin, end;
    find_image_tag_in_prompt(prompt, begin, end);
    return (begin != std::string::npos);
}

// replaces the base64 image tag in the prompt with `replacement`
static llava_image_embed * llava_image_embed_make_with_prompt_base64(struct clip_ctx * ctx_clip, int n_threads,
                                                                     const std::string & prompt) {
    size_t img_base64_str_start, img_base64_str_end;
    find_image_tag_in_prompt(prompt, img_base64_str_start, img_base64_str_end);
    if (img_base64_str_start == std::string::npos || img_base64_str_end == std::string::npos) {
        LOG_ERR("%s: invalid base64 image tag. must be %s<base64 byte string>%s\n", __func__, IMG_BASE64_TAG_BEGIN,
                IMG_BASE64_TAG_END);
        return NULL;
    }

    auto base64_bytes_start = img_base64_str_start + strlen(IMG_BASE64_TAG_BEGIN);
    auto base64_bytes_count = img_base64_str_end - base64_bytes_start;
    auto base64_str         = prompt.substr(base64_bytes_start, base64_bytes_count);

    auto required_bytes = base64::required_encode_size(base64_str.size());
    auto img_bytes      = std::vector<unsigned char>(required_bytes);
    base64::decode(base64_str.begin(), base64_str.end(), img_bytes.begin());

    auto embed = llava_image_embed_make_with_bytes(ctx_clip, n_threads, img_bytes.data(), img_bytes.size());
    if (!embed) {
        LOG_ERR("%s: could not load image from base64 string.\n", __func__);
        return NULL;
    }

    return embed;
}

static std::string remove_image_from_prompt(const std::string & prompt, const char * replacement = "") {
    size_t begin, end;
    find_image_tag_in_prompt(prompt, begin, end);
    if (begin == std::string::npos || end == std::string::npos) {
        return prompt;
    }
    auto pre  = prompt.substr(0, begin);
    auto post = prompt.substr(end + strlen(IMG_BASE64_TAG_END));
    return pre + replacement + post;
}

struct llava_context {
    struct clip_ctx *      ctx_clip  = NULL;
    struct llama_context * ctx_llama = NULL;
    struct llama_model *   model     = NULL;
};

static void print_usage(int, char ** argv) {
    LOG("\n example usage:\n");
    LOG("\n     %s -m <llava-v1.5-7b/ggml-model-q5_k.gguf> --mmproj <llava-v1.5-7b/mmproj-model-f16.gguf> --image "
        "<path/to/an/image.jpg> --image <path/to/another/image.jpg> [--temp 0.1] [-p \"describe the image in "
        "detail.\"]\n",
        argv[0]);
    LOG("\n note: a lower temperature value like 0.1 is recommended for better quality.\n");
}

static inline ocr_context * ocr_create_context(int argc, char ** argv) {
    auto * ctx = static_cast<ocr_context *>(malloc(sizeof(ocr_context)));
    if (!ctx) {
        return NULL;
    }
    // common_params p;
    // ctx->params = static_cast<common_params *>(malloc(sizeof(common_params)));
    ctx->params = new common_params;

    // if (!common_params_parse(argc, argv, *ctx->params, LLAMA_EXAMPLE_MAIN, [](int, char **) {})) {
    if (!common_params_parse(argc, argv, *ctx->params, LLAMA_EXAMPLE_COMMON, print_usage)) {
        // free(ctx->params);
        free(ctx);
        return NULL;
    }
    common_init();
    llama_numa_init(ctx->params->numa);

    llama_model_params   model_params = common_model_params_to_llama(*ctx->params);
    llama_context_params ctx_params   = common_context_params_to_llama(*ctx->params);

    ctx_params.n_ctx = ctx->params->n_ctx < 2048 ? 2048 : ctx->params->n_ctx;

    llama_model * model = llama_load_model_from_file(ctx->params->model.c_str(), model_params);
    if (model == NULL) {
        // free(ctx->params);
        delete ctx->params;
        free(ctx);
        return NULL;
    }

    llama_context * ctx_llama = llama_new_context_with_model(model, ctx_params);
    if (ctx_llama == NULL) {
        llama_free_model(model);
        // free(ctx->params);
        delete ctx->params;
        free(ctx);
        return NULL;
    }
    ctx->model = model;
    ctx->ctx   = ctx_llama;
    return ctx;
}

void * ocr_init(int argc, char ** argv) {
    ggml_time_init();
    llama_backend_init();
    return ocr_create_context(argc, argv);
}

int ocr_free(void * ctx) {
    if (ctx == NULL) {
        return -1;
    }
    auto * ocr_ctx = static_cast<ocr_context *>(ctx);
    // free(ocr_ctx->params);
    delete ocr_ctx->params;
    llama_free(ocr_ctx->ctx);
    llama_free_model(ocr_ctx->model);
    free(ocr_ctx);
    llama_backend_free();
    return 0;
}

int ocr_free_result(ocr_result * result) {
    if (result == NULL) {
        return -1;
    }
    if (result->result != NULL) {
        free(result->result);
    }
    if (result->error != NULL) {
        free(result->error);
    }
    free(result);
    return 0;
}

bool got_eval_image_embed(llama_context * ctx_llama, llava_image_embed * got_embed, const int32_t n_batch,
                          int * n_past) {
    const int  n_embd     = llama_n_embd(llama_get_model(ctx_llama));
    const auto img_tokens = got_embed->n_image_pos;
    for (int i = 0; i < img_tokens; i += n_batch) {
        int n_eval = img_tokens - i;
        n_eval     = std::min(n_eval, n_batch);

        llama_batch batch = {
            n_eval,                             // n_tokens
            nullptr,                            // token
            (got_embed->embed + (i * n_embd)),  // embed
            nullptr,                            // pos
            nullptr,                            // n_seq_id
            nullptr,                            // seq_id
            nullptr,                            // logits
        };
        if (llama_decode(ctx_llama, batch)) {
            LOG_ERR("%s : failed to eval\n", __func__);
            return false;
        }
        *n_past += n_eval;
    }
    return true;
}

ocr_result * ocr_run(void * ctx, const float * image_embeds, const int n_embeds, const GOT_TYPE got_type) {
    constexpr auto dim = 1024;
    constexpr auto system_prompt =
        "<|im_start|>system\n"
        "You should follow the instructions carefully and explain your answers in detail.<|im_end|><|im_start|>user<img>";
    constexpr auto ocr_crop_format_prompt =
        "</img>\nOCR with format upon the patch reference: <|im_end|><|im_start|>assistant";
    constexpr auto ocr_crop_normal_prompt = "</img>\nOCR upon the patch reference: <|im_end|><|im_start|>assistant";
    constexpr auto ocr_normal_prompt      = "</img>\nOCR: <|im_end|><|im_start|>assistant";
    constexpr auto ocr_format_prompt      = "</img>\nOCR with format: <|im_end|><|im_start|>assistant";
    std::string    user_prompt;
    switch (got_type) {
        case GOT_OCR_TYPE:
            user_prompt = ocr_normal_prompt;
            break;
        case GOT_FORMAT_TYPE:
            user_prompt = ocr_format_prompt;
            break;
        case GOT_CROP_OCR_TYPE:
            user_prompt = ocr_crop_normal_prompt;
            break;
        case GOT_CROP_FORMAT_TYPE:
            user_prompt = ocr_crop_format_prompt;
            break;
        default:
            return nullptr;
    }
    // user_prompt = "What's your name?";
    auto *    ocr_ctx     = static_cast<ocr_context *>(ctx);
    auto *    params      = ocr_ctx->params;
    auto *    ctx_llama   = ocr_ctx->ctx;
    auto *    model       = ocr_ctx->model;
    int       n_past      = 0;
    const int max_tgt_len = params->n_predict < 0 ? 256 : params->n_predict;
    auto *    result      = static_cast<ocr_result *>(malloc(sizeof(ocr_result)));
    result->result        = NULL;
    result->error         = NULL;

    auto * got_embed       = static_cast<llava_image_embed *>(malloc(sizeof(llava_image_embed)));
    got_embed->n_image_pos = n_embeds;
    got_embed->embed       = static_cast<float *>(malloc(n_embeds * dim * sizeof(float)));
    memcpy(got_embed->embed, image_embeds, n_embeds * dim * sizeof(float));

    eval_string(ctx_llama, system_prompt, params->n_batch, &n_past, false);
    // got_eval_image_embed(ctx_llama, got_embed, params->n_batch, &n_past);
    llava_eval_image_embed(ctx_llama, got_embed, params->n_batch, &n_past);
    eval_string(ctx_llama, user_prompt.c_str(), params->n_batch, &n_past, false);

    params->sampling.temp        = -1.0;
    struct common_sampler * smpl = common_sampler_init(model, params->sampling);
    if (!smpl) {
        fprintf(stderr, "Could not init ctx for the sampler\n");
        constexpr auto * err_msg = "Could not init ctx for the sampler\n";
        result->error            = static_cast<char *>(malloc(strlen(err_msg) + 1));
        strcpy(result->error, err_msg);

        ocr_cleanup_ctx(ctx);
        llava_image_embed_free(got_embed);
        return result;
    }
    std::string response;

    for (int i = 0; i < max_tgt_len; i++) {
        const char * tmp = sample(smpl, ctx_llama, &n_past);
        // fprintf(stderr, "token: %s\n", tmp);
        // fflush(stderr);
        if (strcmp(tmp, "</s>") == 0) {
            break;
        }
        if (strstr(tmp, "###")) {
            break;  // Yi-VL behavior
        }
        if (strstr(response.c_str(), "<|im_end|>")) {
            break;  // Yi-34B llava-1.6 - for some reason those decode not as the correct token (tokenizer works)
        }
        if (strstr(response.c_str(), "<|im_start|>")) {
            break;  // Yi-34B llava-1.6
        }
        if (strstr(response.c_str(), "USER:")) {
            break;  // mistral llava-1.6
        }
        response += tmp;
    }
    result->result = static_cast<char *>(malloc(response.length() + 1));
    strcpy(result->result, response.c_str());
    common_sampler_free(smpl);
    ocr_cleanup_ctx(ctx);
    llava_image_embed_free(got_embed);
    // fprintf(stderr, "response total:\033[31m<* %s *>\033[0m\n", response.c_str());
    // fflush(stderr);
    return result;
}

int ocr_cleanup_ctx(void * ctx) {
    if (ctx == NULL) {
        return -1;
    }
    auto * ocr_ctx = static_cast<ocr_context *>(ctx);
    auto * params  = ocr_ctx->params;

    llama_context_params ctx_params = common_context_params_to_llama(*params);
    ctx_params.n_ctx                = params->n_ctx < 2048 ? 2048 : params->n_ctx;

    llama_free(ocr_ctx->ctx);
    ocr_ctx->ctx = llama_new_context_with_model(ocr_ctx->model, ctx_params);
    return 0;
}