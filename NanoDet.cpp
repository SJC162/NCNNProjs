// Tencent is pleased to support the open source community by making ncnn available.
//
// Copyright (C) 2021 THL A29 Limited, a Tencent company. All rights reserved.
//
// Licensed under the BSD 3-Clause License (the "License"); you may not use this file except
// in compliance with the License. You may obtain a copy of the License at
//
// https://opensource.org/licenses/BSD-3-Clause
//
// Unless required by applicable law or agreed to in writing, software distributed
// under the License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
// CONDITIONS OF ANY KIND, either express or implied. See the License for the
// specific language governing permissions and limitations under the License.

#include "nanodet.h"

static inline float intersection_area(const NANOObject& a, const NANOObject& b)
{
    cv::Rect_<float> inter = a.rect & b.rect;
    return inter.area();
}

static void qsort_descent_inplace(std::vector<NANOObject>& faceobjects, int left, int right)
{
    int i = left;
    int j = right;
    float p = faceobjects[(left + right) / 2].prob;

    while (i <= j)
    {
        while (faceobjects[i].prob > p)
            i++;

        while (faceobjects[j].prob < p)
            j--;

        if (i <= j)
        {
            // swap
            std::swap(faceobjects[i], faceobjects[j]);

            i++;
            j--;
        }
    }

    //     #pragma omp parallel sections
    {
        //         #pragma omp section
        {
            if (left < j) qsort_descent_inplace(faceobjects, left, j);
        }
        //         #pragma omp section
        {
            if (i < right) qsort_descent_inplace(faceobjects, i, right);
        }
    }
}

static void qsort_descent_inplace(std::vector<NANOObject>& faceobjects)
{
    if (faceobjects.empty())
        return;

    qsort_descent_inplace(faceobjects, 0, faceobjects.size() - 1);
}

static void nms_sorted_bboxes(const std::vector<NANOObject>& faceobjects, std::vector<int>& picked, float nms_threshold)
{
    picked.clear();

    const int n = faceobjects.size();

    std::vector<float> areas(n);
    for (int i = 0; i < n; i++)
    {
        areas[i] = faceobjects[i].rect.width * faceobjects[i].rect.height;
    }

    for (int i = 0; i < n; i++)
    {
        const NANOObject& a = faceobjects[i];

        int keep = 1;
        for (int j = 0; j < (int)picked.size(); j++)
        {
            const NANOObject& b = faceobjects[picked[j]];

            // intersection over union
            float inter_area = intersection_area(a, b);
            float union_area = areas[i] + areas[picked[j]] - inter_area;
            // float IoU = inter_area / union_area
            if (inter_area / union_area > nms_threshold)
                keep = 0;
        }

        if (keep)
            picked.push_back(i);
    }
}

static void generate_proposals(const ncnn::Mat& cls_pred, const ncnn::Mat& dis_pred, int stride, const ncnn::Mat& in_pad, float prob_threshold, std::vector<NANOObject>& objects)
{
    const int num_grid = cls_pred.h;

    int num_grid_x;
    int num_grid_y;
    if (in_pad.w > in_pad.h)
    {
        num_grid_x = in_pad.w / stride;
        num_grid_y = num_grid / num_grid_x;
    }
    else
    {
        num_grid_y = in_pad.h / stride;
        num_grid_x = num_grid / num_grid_y;
    }

    const int num_class = cls_pred.w;
    const int reg_max_1 = dis_pred.w / 4;

    for (int i = 0; i < num_grid_y; i++)
    {
        for (int j = 0; j < num_grid_x; j++)
        {
            const int idx = i * num_grid_x + j;

            const float* scores = cls_pred.row(idx);

            // find label with max score
            int label = -1;
            float score = -FLT_MAX;
            for (int k = 0; k < num_class; k++)
            {
                if (scores[k] > score)
                {
                    label = k;
                    score = scores[k];
                }
            }

            if (score >= prob_threshold)
            {
                ncnn::Mat bbox_pred(reg_max_1, 4, (void*)dis_pred.row(idx));
                {
                    ncnn::Layer* softmax = ncnn::create_layer("Softmax");

                    ncnn::ParamDict pd;
                    pd.set(0, 1); // axis
                    pd.set(1, 1);
                    softmax->load_param(pd);

                    ncnn::Option opt;
                    opt.num_threads = 1;
                    opt.use_packing_layout = false;

                    softmax->create_pipeline(opt);

                    softmax->forward_inplace(bbox_pred, opt);

                    softmax->destroy_pipeline(opt);

                    delete softmax;
                }

                float pred_ltrb[4];
                for (int k = 0; k < 4; k++)
                {
                    float dis = 0.f;
                    const float* dis_after_sm = bbox_pred.row(k);
                    for (int l = 0; l < reg_max_1; l++)
                    {
                        dis += l * dis_after_sm[l];
                    }

                    pred_ltrb[k] = dis * stride;
                }

                float pb_cx = (j + 0.5f) * stride;
                float pb_cy = (i + 0.5f) * stride;

                float x0 = pb_cx - pred_ltrb[0];
                float y0 = pb_cy - pred_ltrb[1];
                float x1 = pb_cx + pred_ltrb[2];
                float y1 = pb_cy + pred_ltrb[3];

                NANOObject obj;
                obj.rect.x = x0;
                obj.rect.y = y0;
                obj.rect.width = x1 - x0;
                obj.rect.height = y1 - y0;
                obj.label = label;
                obj.prob = score;

                objects.push_back(obj);
            }
        }
    }
}

//nanodet-ELite2_512 nanodet-m-416  nanodet_m_1.5x_416_ship-opt-fp16
NanoDet::NanoDet(QObject *parent) : ncnnModelBase("nanodet-m-416", parent)
{
//    blob_pool_allocator.set_size_compare_ratio(0.f);
//    workspace_pool_allocator.set_size_compare_ratio(0.f);

    load(NANO_M416);
}

NanoDet::~NanoDet()
{

}

int NanoDet::load(NANODET_TYPE type)
{
//    blob_pool_allocator.clear();
//    workspace_pool_allocator.clear();

//    ncnn::set_cpu_powersave(2);
//    ncnn::set_omp_num_threads(ncnn::get_big_cpu_count());

//    net.opt = ncnn::Option();
//    net.opt.num_threads = ncnn::get_big_cpu_count();
//    net.opt.blob_allocator = &blob_pool_allocator;
//    net.opt.workspace_allocator = &workspace_pool_allocator;

    switch (type)
    {
    case NANO_M:
        target_size = 320;
        mean_vals[0] = 103.53f;
        mean_vals[1] = 116.28f;
        mean_vals[2] = 123.675f;
        norm_vals[0] = 1.f / 57.375f;
        norm_vals[1] = 1.f / 57.12f;
        norm_vals[2] = 1.f / 58.395f;
        break;
    case NANO_M416:
    case NANO_REPVGG416:
    case NANO_G:
        target_size = 416;
        mean_vals[0] = 103.53f;
        mean_vals[1] = 116.28f;
        mean_vals[2] = 123.675f;
        norm_vals[0] = 1.f / 57.375f;
        norm_vals[1] = 1.f / 57.12f;
        norm_vals[2] = 1.f / 58.395f;
        break;
    case NANO_E320:
        target_size = 320;
        mean_vals[0] = 127.f;
        mean_vals[1] = 127.f;
        mean_vals[2] = 127.f;
        norm_vals[0] = 1.f / 128.f;
        norm_vals[1] = 1.f / 128.f;
        norm_vals[2] = 1.f / 128.f;
        break;
    case NANO_E416:
        target_size = 416;
        mean_vals[0] = 127.f;
        mean_vals[1] = 127.f;
        mean_vals[2] = 127.f;
        norm_vals[0] = 1.f / 128.f;
        norm_vals[1] = 1.f / 128.f;
        norm_vals[2] = 1.f / 128.f;
        break;
    case NANO_E512:
        target_size = 512;
        mean_vals[0] = 127.f;
        mean_vals[1] = 127.f;
        mean_vals[2] = 127.f;
        norm_vals[0] = 1.f / 128.f;
        norm_vals[1] = 1.f / 128.f;
        norm_vals[2] = 1.f / 128.f;
        break;
    default:
        throw invalid_argument("NANO type error.");
        break;
    }

    return 0;
}

bool NanoDet::predict(cv::Mat & frame)
{
    double ncnnstart = ncnn::get_current_time();
    std::vector<NANOObject> objects;
    QTime time;
    time.start();
    detect(frame, objects, 0.4, 0.5);
    qDebug()<<"QTime: "<<time.elapsed()<<"ms";
//    qDebug()<<"objects: "<<objects.size();
    draw(frame, objects);
    double ncnnfinish = ncnn::get_current_time();
    double model_time = (double)(ncnnfinish - ncnnstart) / 1000;
    putText(frame, to_string(model_time), Point(frame.cols/2, frame.rows/2), FONT_HERSHEY_SIMPLEX, 1, Scalar(0, 255, 0), 1);

    return true;
}


int NanoDet::detect(const cv::Mat& rgb, std::vector<NANOObject>& objects, float prob_threshold, float nms_threshold)
{
    int width = rgb.cols;
    int height = rgb.rows;

    // pad to multiple of 32
    int w = width;
    int h = height;
    float scale = 1.f;
    if (w > h)
    {
        scale = (float)target_size / w;
        w = target_size;
        h = h * scale;
    }
    else
    {
        scale = (float)target_size / h;
        h = target_size;
        w = w * scale;
    }

    ncnn::Mat in = ncnn::Mat::from_pixels_resize(rgb.data, ncnn::Mat::PIXEL_RGB2BGR, width, height, w, h);

    // pad to target_size rectangle
    int wpad = (w + 31) / 32 * 32 - w;
    int hpad = (h + 31) / 32 * 32 - h;
    ncnn::Mat in_pad;
    ncnn::copy_make_border(in, in_pad, hpad / 2, hpad - hpad / 2, wpad / 2, wpad - wpad / 2, ncnn::BORDER_CONSTANT, 0.f);

    in_pad.substract_mean_normalize(mean_vals, norm_vals);

    ncnn::Extractor ex = net.create_extractor();
    ex.set_light_mode(true);
    ex.set_num_threads(4);

#if NCNN_VULKAN
    qDebug()<<"gpu count: "<<ncnn::get_gpu_count();
    if(ncnn::get_gpu_count() > 0)
    {
        net.set_vulkan_device(0);
        ex.set_vulkan_compute(true);
    }
    else
        ex.set_vulkan_compute(false);
#endif
    ex.input("input.1", in_pad);

    std::vector<NANOObject> proposals;

    // stride 8
    {
        ncnn::Mat cls_pred;
        ncnn::Mat dis_pred;
        ex.extract("cls_pred_stride_8", cls_pred);
        ex.extract("dis_pred_stride_8", dis_pred);

        std::vector<NANOObject> objects8;
        generate_proposals(cls_pred, dis_pred, 8, in_pad, prob_threshold, objects8);

        proposals.insert(proposals.end(), objects8.begin(), objects8.end());
    }

    // stride 16
    {
        ncnn::Mat cls_pred;
        ncnn::Mat dis_pred;
        ex.extract("cls_pred_stride_16", cls_pred);
        ex.extract("dis_pred_stride_16", dis_pred);

        std::vector<NANOObject> objects16;
        generate_proposals(cls_pred, dis_pred, 16, in_pad, prob_threshold, objects16);

        proposals.insert(proposals.end(), objects16.begin(), objects16.end());
    }

    // stride 32
    {
        ncnn::Mat cls_pred;
        ncnn::Mat dis_pred;
        ex.extract("cls_pred_stride_32", cls_pred);
        ex.extract("dis_pred_stride_32", dis_pred);

        std::vector<NANOObject> objects32;
        generate_proposals(cls_pred, dis_pred, 32, in_pad, prob_threshold, objects32);

        proposals.insert(proposals.end(), objects32.begin(), objects32.end());
    }

    // sort all proposals by score from highest to lowest
    qsort_descent_inplace(proposals);

    // apply nms with nms_threshold
    std::vector<int> picked;
    nms_sorted_bboxes(proposals, picked, nms_threshold);

    int count = picked.size();

    objects.resize(count);
    for (int i = 0; i < count; i++)
    {
        objects[i] = proposals[picked[i]];

        // adjust offset to original unpadded
        float x0 = (objects[i].rect.x - (wpad / 2)) / scale;
        float y0 = (objects[i].rect.y - (hpad / 2)) / scale;
        float x1 = (objects[i].rect.x + objects[i].rect.width - (wpad / 2)) / scale;
        float y1 = (objects[i].rect.y + objects[i].rect.height - (hpad / 2)) / scale;

        // clip
        x0 = std::max(std::min(x0, (float)(width - 1)), 0.f);
        y0 = std::max(std::min(y0, (float)(height - 1)), 0.f);
        x1 = std::max(std::min(x1, (float)(width - 1)), 0.f);
        y1 = std::max(std::min(y1, (float)(height - 1)), 0.f);

        objects[i].rect.x = x0;
        objects[i].rect.y = y0;
        objects[i].rect.width = x1 - x0;
        objects[i].rect.height = y1 - y0;
    }

    // sort objects by area
    struct
    {
        bool operator()(const NANOObject& a, const NANOObject& b) const
        {
            return a.rect.area() > b.rect.area();
        }
    } objects_area_greater;
    std::sort(objects.begin(), objects.end(), objects_area_greater);

    return 0;
}

int NanoDet::draw(cv::Mat& rgb, const std::vector<NANOObject>& objects)
{
    static const char* class_names[] = {
        "person", "bicycle", "car", "motorcycle", "airplane", "bus", "train", "truck", "boat", "traffic light",
        "fire hydrant", "stop sign", "parking meter", "bench", "bird", "cat", "dog", "horse", "sheep", "cow",
        "elephant", "bear", "zebra", "giraffe", "backpack", "umbrella", "handbag", "tie", "suitcase", "frisbee",
        "skis", "snowboard", "sports ball", "kite", "baseball bat", "baseball glove", "skateboard", "surfboard",
        "tennis racket", "bottle", "wine glass", "cup", "fork", "knife", "spoon", "bowl", "banana", "apple",
        "sandwich", "orange", "broccoli", "carrot", "hot dog", "pizza", "donut", "cake", "chair", "couch",
        "potted plant", "bed", "dining table", "toilet", "tv", "laptop", "mouse", "remote", "keyboard", "cell phone",
        "microwave", "oven", "toaster", "sink", "refrigerator", "book", "clock", "vase", "scissors", "teddy bear",
        "hair drier", "toothbrush"
    };

    static const unsigned char colors[19][3] = {
        { 54,  67, 244},
        { 99,  30, 233},
        {176,  39, 156},
        {183,  58, 103},
        {181,  81,  63},
        {243, 150,  33},
        {244, 169,   3},
        {212, 188,   0},
        {136, 150,   0},
        { 80, 175,  76},
        { 74, 195, 139},
        { 57, 220, 205},
        { 59, 235, 255},
        {  7, 193, 255},
        {  0, 152, 255},
        { 34,  87, 255},
        { 72,  85, 121},
        {158, 158, 158},
        {139, 125,  96}
    };

    int color_index = 0;

    for (size_t i = 0; i < objects.size(); i++)
    {
        const NANOObject& obj = objects[i];

//         fprintf(stderr, "%d = %.5f at %.2f %.2f %.2f x %.2f\n", obj.label, obj.prob,
//                 obj.rect.x, obj.rect.y, obj.rect.width, obj.rect.height);

        const unsigned char* color = colors[color_index % 19];
        color_index++;

        cv::Scalar cc(color[0], color[1], color[2]);

        cv::rectangle(rgb, obj.rect, cc, 2);

        char text[256];
        sprintf(text, "%s %.1f%%", class_names[obj.label], obj.prob * 100);

        int baseLine = 0;
        cv::Size label_size = cv::getTextSize(text, cv::FONT_HERSHEY_SIMPLEX, 0.5, 1, &baseLine);

        int x = obj.rect.x;
        int y = obj.rect.y - label_size.height - baseLine;
        if (y < 0)
            y = 0;
        if (x + label_size.width > rgb.cols)
            x = rgb.cols - label_size.width;

        cv::rectangle(rgb, cv::Rect(cv::Point(x, y), cv::Size(label_size.width, label_size.height + baseLine)), cc, -1);

        cv::Scalar textcc = (color[0] + color[1] + color[2] >= 381) ? cv::Scalar(0, 0, 0) : cv::Scalar(255, 255, 255);

        cv::putText(rgb, text, cv::Point(x, y + label_size.height), cv::FONT_HERSHEY_SIMPLEX, 0.5, textcc, 1);
    }

    return 0;
}
