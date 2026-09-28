#include "camera.hpp"
#include <camera/NdkCameraManager.h>
#include <media/NdkImageReader.h>
#include <android/log.h>
#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <limits>
#include <utility>

// Same manager -> device -> reader -> session -> request flow as:
// https://sisik.eu/blog/android/ndk/camera
struct Camera::CaptureState {
    ACameraDevice* device = nullptr;
    AImageReader* reader = nullptr;
    ACaptureSessionOutputContainer* outputs = nullptr;
    ACaptureSessionOutput* output = nullptr;
    ACameraOutputTarget* target = nullptr;
    ACaptureRequest* request = nullptr;
    ACameraCaptureSession* session = nullptr;
    int sensorOrientation = 0;
    // Only this value is written by Android's callback thread.
    std::atomic<int> deviceError{0};
};

std::optional<Camera> Camera::init()
{
    Camera camera;
    if (!camera._cameraManager)
        return std::nullopt;
    return camera;
}

Camera::Camera()
{
    this->_cameraManager = ACameraManager_create();
};

Camera::Camera(Camera&& other) noexcept
{
    this->_cameraManager = std::exchange(other._cameraManager, nullptr);
    this->_capture = std::move(other._capture);
    this->_lastError = std::move(other._lastError);
};

Camera& Camera::operator=(Camera&& other) noexcept
{
    if (this != &other) {
        close();
        if (this->_cameraManager)
            ACameraManager_delete(this->_cameraManager);
        this->_cameraManager = std::exchange(other._cameraManager, nullptr);
        this->_capture = std::move(other._capture);
        this->_lastError = std::move(other._lastError);
    }
    return *this;
};

Camera::~Camera()
{
    close();
    if (this->_cameraManager != nullptr) {
       ACameraManager_delete(this->_cameraManager);
    }
};

std::string getBackFacingCamId(ACameraManager* cameraManager)
{
    ACameraIdList* cameraIds = nullptr;
    if (ACameraManager_getCameraIdList(cameraManager, &cameraIds) != ACAMERA_OK)
        return {};

    std::string backId;

    for (int i = 0; i < cameraIds->numCameras; ++i)
    {
        const char* id = cameraIds->cameraIds[i];

        ACameraMetadata* metadataObj = nullptr;
        if (ACameraManager_getCameraCharacteristics(cameraManager, id, &metadataObj)
                != ACAMERA_OK)
            continue;

        ACameraMetadata_const_entry lensInfo{};
        const auto status = ACameraMetadata_getConstEntry(
            metadataObj, ACAMERA_LENS_FACING, &lensInfo);
        const bool isBack = status == ACAMERA_OK && lensInfo.count > 0 &&
            lensInfo.data.u8[0] == ACAMERA_LENS_FACING_BACK;
        ACameraMetadata_free(metadataObj);

        // Found a back-facing camera
        if (isBack)
        {
            backId = id;
            break;
        }
    }

    ACameraManager_deleteCameraIdList(cameraIds);

    return backId;
};

bool Camera::open()
{
    if (isOpen())
        return true;
    close();
    _lastError.clear();

    auto fail = [this](const char* operation, int status = 0) {
        _lastError = std::string(operation) + " failed (" + std::to_string(status) + ")";
        __android_log_print(ANDROID_LOG_ERROR, "AirgapCamera", "%s", _lastError.c_str());
        close(); // Also releases partially constructed captures.
        return false;
    };

    if (!_cameraManager)
        return fail("Camera manager unavailable");
    const auto id = getBackFacingCamId(_cameraManager);
    if (id.empty())
        return fail("Find back-facing camera");

    _capture = std::make_unique<CaptureState>();
    auto& capture = *_capture;

    ACameraMetadata* metadata = nullptr;
    auto status = ACameraManager_getCameraCharacteristics(_cameraManager, id.c_str(), &metadata);
    if (status != ACAMERA_OK)
        return fail("Read camera characteristics", status);

    ACameraMetadata_const_entry entry{};
    if (ACameraMetadata_getConstEntry(metadata, ACAMERA_SENSOR_ORIENTATION, &entry)
            == ACAMERA_OK && entry.count > 0)
        capture.sensorOrientation = entry.data.i32[0];

    // Prefer a modest preview size, but only choose an advertised YUV output.
    int width = 0, height = 0;
    int64_t bestDistance = std::numeric_limits<int64_t>::max();
    if (ACameraMetadata_getConstEntry(metadata,
            ACAMERA_SCALER_AVAILABLE_STREAM_CONFIGURATIONS, &entry) == ACAMERA_OK) {
        for (uint32_t i = 0; i + 3 < entry.count; i += 4) {
            const auto* config = entry.data.i32 + i;
            if (config[0] != AIMAGE_FORMAT_YUV_420_888 || config[3] != 0 ||
                    config[1] <= 0 || config[2] <= 0)
                continue;
            const int64_t distance = std::abs(int64_t(config[1]) * config[2] - 640 * 480);
            if (distance < bestDistance) {
                width = config[1];
                height = config[2];
                bestDistance = distance;
            }
        }
    }
    ACameraMetadata_free(metadata);
    if (width == 0)
        return fail("Find supported YUV size");

    ACameraDevice_StateCallbacks deviceCallbacks{};
    deviceCallbacks.context = &capture;
    deviceCallbacks.onDisconnected = [](void* context, ACameraDevice*) {
        static_cast<CaptureState*>(context)->deviceError.store(ACAMERA_ERROR_CAMERA_DISCONNECTED);
    };
    deviceCallbacks.onError = [](void* context, ACameraDevice*, int error) {
        static_cast<CaptureState*>(context)->deviceError.store(error);
    };
    status = ACameraManager_openCamera(_cameraManager, id.c_str(), &deviceCallbacks, &capture.device);
    if (status != ACAMERA_OK)
        return fail("Open camera (check camera permission)", status);

    // The UI polls the latest image. No detached worker or queued frame ownership.
    auto mediaStatus = AImageReader_new(width, height, AIMAGE_FORMAT_YUV_420_888, 3, &capture.reader);
    if (mediaStatus != AMEDIA_OK)
        return fail("Create image reader", mediaStatus);
    ANativeWindow* window = nullptr; // Borrowed from reader; do not release separately.
    mediaStatus = AImageReader_getWindow(capture.reader, &window);
    if (mediaStatus != AMEDIA_OK)
        return fail("Get image reader window", mediaStatus);

    status = ACaptureSessionOutputContainer_create(&capture.outputs);
    if (status != ACAMERA_OK) return fail("Create output container", status);
    status = ACaptureSessionOutput_create(window, &capture.output);
    if (status != ACAMERA_OK) return fail("Create output", status);
    status = ACaptureSessionOutputContainer_add(capture.outputs, capture.output);
    if (status != ACAMERA_OK) return fail("Add output", status);
    status = ACameraOutputTarget_create(window, &capture.target);
    if (status != ACAMERA_OK) return fail("Create output target", status);
    status = ACameraDevice_createCaptureRequest(capture.device, TEMPLATE_PREVIEW, &capture.request);
    if (status != ACAMERA_OK) return fail("Create preview request", status);
    status = ACaptureRequest_addTarget(capture.request, capture.target);
    if (status != ACAMERA_OK) return fail("Add preview target", status);

    // Session callbacks deliberately hold no Camera pointer: onClosed can be late.
    ACameraCaptureSession_stateCallbacks sessionCallbacks{};
    sessionCallbacks.onClosed = [](void*, ACameraCaptureSession*) {};
    sessionCallbacks.onReady = [](void*, ACameraCaptureSession*) {};
    sessionCallbacks.onActive = [](void*, ACameraCaptureSession*) {};
    status = ACameraDevice_createCaptureSession(capture.device, capture.outputs,
                                                &sessionCallbacks, &capture.session);
    if (status != ACAMERA_OK) return fail("Create capture session", status);
    status = ACameraCaptureSession_setRepeatingRequest(capture.session, nullptr, 1,
                                                      &capture.request, nullptr);
    if (status != ACAMERA_OK) return fail("Start preview", status);
    if (!isOpen()) {
        close();
        return false;
    }
    __android_log_print(ANDROID_LOG_INFO, "AirgapCamera", "Camera opened: %dx%d", width, height);
    return true;
}

void Camera::close()
{
    if (!_capture)
        return;
    auto& capture = *_capture;
    if (capture.deviceError.load() != 0)
        _lastError = lastError();
    if (capture.session)
        ACameraCaptureSession_close(capture.session);
    // Synchronously stop the device before freeing its outputs or callback context.
    if (capture.device)
        ACameraDevice_close(capture.device);
    if (capture.request)
        ACaptureRequest_free(capture.request);
    if (capture.target)
        ACameraOutputTarget_free(capture.target);
    if (capture.outputs)
        ACaptureSessionOutputContainer_free(capture.outputs);
    if (capture.output)
        ACaptureSessionOutput_free(capture.output);
    if (capture.reader)
        AImageReader_delete(capture.reader);
    _capture.reset();
    __android_log_print(ANDROID_LOG_INFO, "AirgapCamera", "Camera closed");
}

bool Camera::isOpen() const
{
    return _capture && _capture->session && _capture->deviceError.load() == 0;
}

std::string Camera::lastError() const
{
    if (_capture) {
        const int error = _capture->deviceError.load();
        if (error == ACAMERA_ERROR_CAMERA_DISCONNECTED)
            return "Camera disconnected";
        if (error != 0)
            return "Camera device error (" + std::to_string(error) + ")";
    }
    return _lastError;
}

std::optional<Camera::Frame> Camera::readFrame()
{
    if (!isOpen()) {
        close(); // Handle a disconnect/error on the owning thread.
        return std::nullopt;
    }
    AImage* image = nullptr;
    const auto status = AImageReader_acquireLatestImage(_capture->reader, &image);
    if (status == AMEDIA_IMGREADER_NO_BUFFER_AVAILABLE)
        return std::nullopt;
    if (status != AMEDIA_OK) {
        _lastError = "Read camera frame failed (" + std::to_string(status) + ")";
        close();
        return std::nullopt;
    }
    std::unique_ptr<AImage, decltype(&AImage_delete)> ownedImage(image, AImage_delete);

    Frame frame;
    frame.sensorOrientation = _capture->sensorOrientation;
    uint8_t* planes[3]{};
    int lengths[3]{}, rows[3]{}, pixels[3]{};
    bool valid = AImage_getWidth(image, &frame.width) == AMEDIA_OK &&
                 AImage_getHeight(image, &frame.height) == AMEDIA_OK &&
                 frame.width > 0 && frame.height > 0;
    for (int plane = 0; valid && plane < 3; ++plane) {
        valid = AImage_getPlaneData(image, plane, &planes[plane], &lengths[plane]) == AMEDIA_OK &&
                AImage_getPlaneRowStride(image, plane, &rows[plane]) == AMEDIA_OK &&
                AImage_getPlanePixelStride(image, plane, &pixels[plane]) == AMEDIA_OK;
        const int w = plane == 0 ? frame.width : (frame.width + 1) / 2;
        const int h = plane == 0 ? frame.height : (frame.height + 1) / 2;
        valid = valid && planes[plane] && rows[plane] > 0 && pixels[plane] > 0 &&
            int64_t(h - 1) * rows[plane] + int64_t(w - 1) * pixels[plane] < lengths[plane];
    }
    if (!valid) {
        _lastError = "Invalid YUV camera frame";
        // Release the acquired image before closing its reader.
        ownedImage.reset();
        close();
        return std::nullopt;
    }

    // YUV planes can be padded or interleaved: honor both strides, never assume NV21.
    // Future QR decoding can use the Y plane here before the preview conversion.
    frame.rgba.resize(size_t(frame.width) * frame.height * 4);
    auto channel = [](int value) { return static_cast<uint8_t>(std::clamp(value, 0, 255)); };
    for (int y = 0; y < frame.height; ++y) {
        for (int x = 0; x < frame.width; ++x) {
            const int luma = std::max(0, int(planes[0][y * rows[0] + x * pixels[0]]) - 16);
            const int u = int(planes[1][(y / 2) * rows[1] + (x / 2) * pixels[1]]) - 128;
            const int v = int(planes[2][(y / 2) * rows[2] + (x / 2) * pixels[2]]) - 128;
            auto* rgba = frame.rgba.data() + (size_t(y) * frame.width + x) * 4;
            rgba[0] = channel((298 * luma + 409 * v + 128) >> 8);
            rgba[1] = channel((298 * luma - 100 * u - 208 * v + 128) >> 8);
            rgba[2] = channel((298 * luma + 516 * u + 128) >> 8);
            rgba[3] = 255;
        }
    }
    return frame;
}
