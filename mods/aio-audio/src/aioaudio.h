#ifndef AIOAUDIO_H
#define AIOAUDIO_H

#include <QByteArray>
#include <QElapsedTimer>
#include <QLabel>
#include <QList>
#include <QMap>
#include <QObject>
#include <QPixmap>
#include <QPointer>
#include <QSize>
#include <QString>
#include <QWidget>

class QEvent;
class QMouseEvent;
class QPaintEvent;
class QTimer;

// A plain label that reports taps, from touch events or from the mouse events Qt synthesizes out of them.
class AioTap : public QLabel {
    Q_OBJECT
public:
    explicit AioTap(QWidget *parent);
Q_SIGNALS:
    void tapped();
protected:
    bool event(QEvent *e) Q_DECL_OVERRIDE;
    void mousePressEvent(QMouseEvent *e) Q_DECL_OVERRIDE;
    void mouseReleaseEvent(QMouseEvent *e) Q_DECL_OVERRIDE;
private:
    bool down;
};

// Thin progress line. Interactive: round knob, tap or drag to seek (one repaint per ~2% of travel for e-ink).
class AioSlider : public QWidget {
    Q_OBJECT
public:
    AioSlider(QWidget *parent, bool interactive, int thickness, const QPixmap &knob);
    void setValue(qint64 value, qint64 maximum); // repaints only when the drawn position moves
Q_SIGNALS:
    void seekRequested(qint64 value);
protected:
    bool event(QEvent *e) Q_DECL_OVERRIDE;
    void paintEvent(QPaintEvent *e) Q_DECL_OVERRIDE;
    void mousePressEvent(QMouseEvent *e) Q_DECL_OVERRIDE;
    void mouseMoveEvent(QMouseEvent *e) Q_DECL_OVERRIDE;
    void mouseReleaseEvent(QMouseEvent *e) Q_DECL_OVERRIDE;
private:
    int margin() const;
    int xFor(qint64 v) const;
    qint64 valueAt(qreal x) const;
    void press(qreal x);
    void drag(qreal x);
    void release(qreal x);
    qint64 value, maximum, dragValue;
    bool interactive, dragging;
    int thickness, drawnX;
    QPixmap knob;
};

// Lives on the application, not on the reader: closing the reader deletes the reader's children, so the
// close call must not run inside one of their slots.
class AioCloser : public QObject {
    Q_OBJECT
public:
    explicit AioCloser(QObject *parent) : QObject(parent) {}
public Q_SLOTS:
    void close();
};

// One per open .mp3z (child of the MP3Reader widget, so it dies with it).
class AioAudio : public QObject {
    Q_OBJECT
public:
    explicit AioAudio(QWidget *reader);
    ~AioAudio();
    void setPlayer(QObject *player);
    void setTrack(int index, const QByteArray &mp3);
    void setCover(const QByteArray &mp3);
    bool playPauseText(QLabel *label, const QString &text); // stock "Play"/"Pause": icon instead of text
    void setChapter(const QString &name);

public Q_SLOTS:
    void rewind();
    void ahead();
    void tick();
    void seekTo(qint64 ms);
    void goHome();
    void cycleSpeed();
    void cycleSleep();
    void sleepFired();
    void endOfTrackCheck();
    void userSkip();
    void applyPending();
    void applyRate();
    void onDuration();
    void flashSender();
    void unflash();
    void volumeDown();
    void volumeUp();

private:
    int px(int dragon) const;
    void seek(qint64 delta);
    void showVolume();
    void stepVolume(int delta);
    void flash(QLabel *label);
    void setIcon(QLabel *label, const QPixmap &pm, const QSize &box);
    void updateSleepLabel();
    void loadState();
    void saveState(bool askPlayer);
    QString bookKey() const;

    QPointer<QWidget> reader;
    QPointer<QObject> player;
    QPointer<QLabel> cover, playPause, chapter, book, elapsed, total, footer, sleepText, speed;
    QPointer<AioSlider> slider, bookBar;
    QPointer<QLabel> volumeLabel;
    QTimer *timer, *flashTimer, *sleepTimer;
    QList<QPointer<QLabel> > flashed;
    QPixmap playIcon, pauseIcon;
    QSize playBox, coverBox;
    qreal scale;
    int coverKey;
    bool playing, started, pauseOnPlay;
    int track, trackNo, trackCount;     // track: 0-based key; trackNo/trackCount from ID3 TRCK ("7/15")
    qint64 lastPos, lastDur, pendingSeek, resumeTarget;
    int savedTrack;
    qint64 savedPos;
    QMap<int, qint64> durations;        // learned track lengths, persisted with the position
    QString bookTitle;
    int speedIndex, sleepMode;
    QElapsedTimer sleepClock, skipClock;
};

#endif
