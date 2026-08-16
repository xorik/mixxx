#pragma once

#include <QOpenGLWindow>

class WGLWidget;
class TrackDropTarget;
class DisplayLinkFrameDriver;

/// Helper class used by wglwidgetqopengl

class OpenGLWindow : public QOpenGLWindow {
    Q_OBJECT

  public:
    OpenGLWindow(WGLWidget* pWidget);
    ~OpenGLWindow();

    void widgetDestroyed();

    /// Installs the driver that turns QEvent::UpdateRequest into waveform
    /// frames (VSyncThread::ST_DISPLAY_LINK mode). Only set on the window of
    /// the shared GL context. The caller keeps the ownership and must call
    /// this with nullptr before the driver is destroyed.
    void setFrameDriver(DisplayLinkFrameDriver* pFrameDriver) {
        m_pFrameDriver = pFrameDriver;
    }

  private:
    void initializeGL() override;
    void paintGL() override;
    void resizeGL(int w, int h) override;
    bool event(QEvent* pEv) override;

    WGLWidget* m_pWidget;
    TrackDropTarget* m_pTrackDropTarget;
    DisplayLinkFrameDriver* m_pFrameDriver;
};
