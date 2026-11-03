#pragma once

#include "CodeImage.h"

#include <QDialog>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QSlider;
class QSpinBox;
class QTimer;
class QToolButton;

// "Copy as Image…": a live preview of the picture with its options beside it (background, padding, corners, font
// size, theme, window buttons, file name, line numbers, shadow, QODE mark, resolution), and Copy / Save PNG.
// The options are kept for the next time.
class CodeImageDialog : public QDialog
{
    Q_OBJECT
public:
    explicit CodeImageDialog(CodeImage::Source source, QWidget *parent = nullptr);

    // Opens a dialog for `source` that deletes itself when closed.
    static void show(CodeImage::Source source, QWidget *parent);

protected:
    void resizeEvent(QResizeEvent *event) override;

private:
    void schedule();          // the preview is drawn again shortly (options change in bursts while dragging)
    void refresh();
    void syncVisibility();
    void choosePicture();
    void pickColour(int which);
    void updateSwatches();
    void copy();
    void savePng();

    CodeImage::Source m_source;
    CodeImage::Style m_style;
    QImage m_previewImage;
    QTimer *m_timer;
    QLabel *m_preview, *m_size;
    QComboBox *m_background, *m_theme, *m_scale;
    QWidget *m_coloursRow, *m_pictureRow;
    QToolButton *m_colour1, *m_colour2;
    QSpinBox *m_angle, *m_fontSize;
    QPushButton *m_pictureBtn, *m_copyBtn;
    QSlider *m_padding, *m_radius;
    QLabel *m_paddingValue, *m_radiusValue;
    QCheckBox *m_titleOn, *m_buttons, *m_lineNumbers, *m_shadow, *m_watermark;
    QLineEdit *m_title;
    int m_lastBackground = 0; // the combo entry before "Picture…" (back to it when no picture is chosen)
};
