#pragma once

#include <optional>
#include <memory>
#include <string>
#include <vector>
#include <cstdint>
#include <camera/NdkCameraManager.h>

class Camera {
    public:
    static std::optional<Camera> init();

    // Call these methods from one thread (the UI thread is fine).
    // Permission must be granted before open(). Repeated open/close calls are safe.
    bool open();
    void close();
    bool isOpen() const;
    std::string lastError() const;

    struct Frame {
        int width = 0;
        int height = 0;
        int sensorOrientation = 0; // Clockwise degrees, before display rotation.
        std::vector<uint8_t> rgba; // Owned pixels; valid even after close().
    };

    // Non-blocking: no value when no new frame is ready. Older frames are dropped.
    std::optional<Frame> readFrame();

    // Disable object copying.
    Camera(const Camera& other) = delete;
    Camera& operator=(const Camera& other) = delete;

    Camera(Camera&& other) noexcept;
    Camera& operator=(Camera&& other) noexcept;

    ~Camera();
    private:
    ACameraManager* _cameraManager;
    struct CaptureState;
    // Stable callback context: moving Camera must not move this allocation.
    std::unique_ptr<CaptureState> _capture;
    std::string _lastError;
    explicit Camera();
};
