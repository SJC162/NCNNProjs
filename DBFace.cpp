//
// Created by WZTENG on 2020/09/14 014.
//

#include "DBFace.h"

DBFace::DBFace(QObject *parent) : ncnnModelBase("dbface", parent)
{

}

DBFace::~DBFace()
{

}

bool DBFace::predict(cv::Mat & frame)
{
    double ncnnstart = ncnn::get_current_time();
    std::vector<Obj> boxes = detect(frame, 0.4, 0.6);
//    qDebug()<<"boxes: "<<boxes.size();
//    putText(frame, to_string(boxes.size()), Point(frame.cols/2, frame.rows/2), FONT_HERSHEY_SIMPLEX, 1, Scalar(0, 255, 0), 1);
    for(Obj &boxInfo: boxes)
    {
        Box b = boxInfo.box;
        Landmark mark = boxInfo.landmark;
        qDebug()<<"boxInfo: "<<b.x<<", "<<b.y<<", "<<b.r<<", "<<b.b;
        rectangle(frame, Point(b.x, b.y), Point(b.r, b.b), Scalar(0, 255, 0), 2);
        for(int i=0; i<mark.x.size(); i++)
            circle(frame, Point(mark.x[i], mark.y[i]), 2, Scalar(0, 255, 0), 2);
    }
    double ncnnfinish = ncnn::get_current_time();
    double model_time = (double)(ncnnfinish - ncnnstart) / 1000;
    putText(frame, to_string(model_time), Point(frame.cols/2, frame.rows/2), FONT_HERSHEY_SIMPLEX, 1, Scalar(0, 255, 0), 1);

    return true;
}

std::vector<Obj> DBFace::detect(cv::Mat & image, double threshold, double nms_threshold)
{
    // 图像缩放
    cv::Mat im = pad(image);
    ncnn::Mat in_net = ncnn::Mat::from_pixels(image.data, ncnn::Mat::PIXEL_BGR2RGB, image.cols, image.rows);

    ncnn::Mat in = ncnn::Mat::from_pixels(im.data, ncnn::Mat::PIXEL_BGR2RGB, im.cols, im.rows);
    const float mean_vals_1[3] = {0.485f * 255.0f, 0.456f * 255.0f, 0.406f * 255.0f};
    const float norm_vals_1[3] = {1.0f / 0.229f / 255.0f, 1.0f / 0.224f / 255.0f, 1.0f / 0.225f / 255.0f};
    in.substract_mean_normalize(mean_vals_1, norm_vals_1);

    ncnn::Extractor ex = net.create_extractor();
    ex.input("0", in);
//    ex.set_num_threads(4);
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
    ncnn::Mat landmark, hm, hmPool, tlrb;
    ex.extract("landmark", landmark);
    ex.extract("hm", hm);
    ex.extract("pool_hm", hmPool);
    ex.extract("tlrb", tlrb);

    int hmWeight = hm.w;
    hm = hm.reshape(hm.c * hm.h * hm.w);
    hmPool = hmPool.reshape(hmPool.c * hmPool.w * hmPool.h);
    std::vector<Id> ids;
    //get suspected boxs
    genIds(hm, hmPool, hmWeight, threshold, ids);

    std::vector<Obj> objs;
    //get each box and key point information
    decode(hmWeight, ids, tlrb, landmark, objs);

    return nms(objs, (float) (1 - nms_threshold));  // 注意这里: 1 - iou
}

void DBFace::genIds(ncnn::Mat hm, ncnn::Mat hmPool, int w, double thresh, std::vector<Id> &ids) {
    const float *ptr = hm.channel(0);
    const float *ptrPool = hmPool.channel(0);
    for (int i = 0; i < hm.w; i++) {
        float temp = 0.0;
        if ((ptr[i] - ptrPool[i]) < 0.01) {
            temp = ptr[i];
        }
        if (ptr[i] > thresh) {
            Id temp;
            temp.idx = i % w;
            temp.idy = (int) (i / w);
            temp.score = ptr[i];
            ids.push_back(temp);
        }
    }
}

void DBFace::decode(int w, std::vector<Id> ids, ncnn::Mat tlrb, ncnn::Mat landmark, std::vector<Obj> &objs) {
    for (int i = 0; i < ids.size(); i++) {
        Obj objTemp;
        int cx = ids[i].idx;
        int cy = ids[i].idy;
        double score = ids[i].score;
        std::vector<float> boxTemp;
        //get each box information
        for (int j = 0; j < tlrb.c; j++) {
            const float *ptr = tlrb.channel(j);
            boxTemp.push_back(ptr[w * (cy - 1) + cx]);
        }
        objTemp.box.x = (cx - boxTemp[0]) * STRIDE;
        objTemp.box.y = (cy - boxTemp[1]) * STRIDE;
        objTemp.box.r = (cx + boxTemp[2]) * STRIDE;
        objTemp.box.b = (cy + boxTemp[3]) * STRIDE;
        objTemp.score = score;

        //get key point information
        Landmark lanTemp;
        for (int j = 0; j < 10; j++) {
            const float *ptr = landmark.channel(j);
            if (j < 5) {
                float temp = (myExp(ptr[w * (cy - 1) + cx] * 4) + cx) * STRIDE;
                lanTemp.x.push_back(temp);
            } else {
                float temp = (myExp(ptr[w * (cy - 1) + cx] * 4) + cy) * STRIDE;
                lanTemp.y.push_back(temp);
            }
        }
        objTemp.landmark = lanTemp;
        objs.push_back(objTemp);
    }
}

inline float DBFace::fast_exp(float x) {
    union {
        uint32_t i;
        float f;
    } v{};
    v.i = (1 << 23) * (1.4426950409 * x + 126.93490512f);
    return v.f;
}

inline float DBFace::myExp(float v) {
    float gate = 1;
    float base = exp(1);
    if (abs(v) < gate) {
        return v * base;
    }
    if (v > 0) {
        return fast_exp(v);
    } else {
        return -fast_exp(-v);
    }
}

cv::Mat DBFace::pad(cv::Mat img, int stride) {
    bool hasChange = false;
    int stdw = img.cols;
    if (stdw % stride != 0) {
        stdw += stride - (stdw % stride);
        hasChange = true;
    }
    int stdh = img.rows;
    if (stdh % stride != 0) {
        stdh += stride - (stdh % stride);
        hasChange = true;
    }
    if (hasChange) {
        cv::Mat newImg = cv::Mat::zeros(stdh, stdw, CV_8UC3);
        cv::Rect roi = cv::Rect(0, 0, img.cols, img.rows);
        img.copyTo(newImg(roi));
        return newImg;
    }
    return img;
}

std::vector<Obj> DBFace::nms(std::vector<Obj> objs, float iou) {
    if (objs.size() == 0) {
        return objs;
    }
    sort(objs.begin(), objs.end(), [](Obj a, Obj b) { return a.score < b.score; });

    std::vector<Obj> keep;
    int *flag = new int[objs.size()]();
    for (int i = 0; i < objs.size(); i++) {
        if (flag[i] != 0) {
            continue;
        }
        keep.push_back(objs[i]);
        for (int j = i + 1; j < objs.size(); j++) {
            if (flag[j] == 0 && getIou(objs[i].box, objs[j].box) > iou) {
                flag[j] = 1;
            }
        }
    }
    return keep;
}

float DBFace::getIou(Box a, Box b) {
    float aArea = (a.r - a.x + 1) * (a.b - a.y + 1);
    float bArea = (b.r - b.x + 1) * (b.b - b.y + 1);

    float x1 = a.x > b.x ? a.x : b.x;
    float y1 = a.y > b.y ? a.y : b.y;
    float x2 = a.r < b.r ? a.r : b.r;
    float y2 = a.b < b.b ? a.b : b.b;
    float w = 0.0f > x2 - x1 + 1 ? 0.0f : x2 - x1 + 1;
    float h = 0.0f > y2 - y1 + 1 ? 0.0f : x2 - x1 + 1;
    float area = w * h;

    float iou = area / (aArea + bArea - area);
    return iou;
}
