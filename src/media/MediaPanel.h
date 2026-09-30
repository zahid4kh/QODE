#pragma once

#include <QWidget>

class QGridLayout;
class QLabel;
class QMovie;
class QSlider;
class QSplitter;
class QStackedWidget;
class QToolButton;
class ImageView;
#ifdef QODE_HAS_MULTIMEDIA
class QAudioOutput;
class QMediaPlayer;
class QVideoWidget;
#endif

// Right-hand side panel that previews an image or a video and lists its file details
// (name, size, resolution, absolute/relative path, ...).
class MediaPanel : public QWidget
{
    Q_OBJECT
public:
    explicit MediaPanel(QWidget *parent = nullptr);

    static bool isImage(const QString &path);
    static bool isVideo(const QString &path);
    static bool isMedia(const QString &path) { return isImage(path) || isVideo(path); }

    void setProjectRoot(const QString &root);
    void openMedia(const QString &path);
    void clear();
    QString currentPath() const { return m_path; }

signals:
    void closeRequested();

private:
    void showImage();
    void showVideo();
    void showMessage(const QString &text);
    void refreshInfo();
    void addInfo(const QString &key, const QString &value, bool wrapPath = false);
    void updateTime();
    void showEvent(QShowEvent *event) override;

    QString m_path;
    QString m_root;
    QStringList m_extra; // key/value pairs specific to the loaded media (resolution, codec, ...)

    QLabel *m_title;
    QSplitter *m_split;
    bool m_sized = false;
    QStackedWidget *m_stack;
    ImageView *m_image;
    QLabel *m_message;
    QGridLayout *m_info;
    QWidget *m_controls;
    QToolButton *m_playBtn;
    QToolButton *m_muteBtn;
    QSlider *m_seek;
    QSlider *m_volume;
    QLabel *m_time;
    QMovie *m_movie = nullptr;
#ifdef QODE_HAS_MULTIMEDIA
    QMediaPlayer *m_player;
    QAudioOutput *m_audio;
    QVideoWidget *m_video;
#endif
};
