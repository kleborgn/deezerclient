#ifndef WAVEFORMWIDGET_H
#define WAVEFORMWIDGET_H

#include <QWidget>
#include <QVector>
#include <QColor>

class WaveformWidget : public QWidget
{
    Q_OBJECT

public:
    explicit WaveformWidget(QWidget *parent = nullptr);

    void setPeaks(const QVector<float>& peaks);
    void setPosition(double position); // 0.0 to 1.0
    void setUnplayedColor(const QColor& color);
    void clear();
    bool isDragging() const { return m_dragging; }

    QSize minimumSizeHint() const override;
    QSize sizeHint() const override;

signals:
    void seekRequested(double position); // emitted on mouse release

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void enterEvent(QEnterEvent *event) override;
    void leaveEvent(QEvent *event) override;

private:
    double positionFromX(int x) const;

    QVector<float> m_peaks;
    double m_position;       // current playback position 0.0-1.0
    double m_dragPosition;   // position while dragging
    bool m_dragging;
    bool m_hovering;
    double m_hoverPosition;  // mouse hover position 0.0-1.0
    QColor m_unplayedColor;  // overridable unplayed bar color
};

#endif // WAVEFORMWIDGET_H
