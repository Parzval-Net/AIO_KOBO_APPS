#ifndef AIOAUDIO_H
#define AIOAUDIO_H

#include <QByteArray>
#include <QColor>
#include <QElapsedTimer>
#include <QFont>
#include <QImage>
#include <QLabel>
#include <QList>
#include <QMap>
#include <QObject>
#include <QPixmap>
#include <QPointer>
#include <QSize>
#include <QString>
#include <QStringList>
#include <QWidget>

class QEvent;
class QMouseEvent;
class QPaintEvent;
class QTimer;

// Passive area (header, rows, bars, cover box): takes every touch and mouse event, so a tap that misses a button
// never falls through to Nickel's reading view (menus, page turns, full refreshes). Optional opaque fill.
class AioPane : public QWidget {
    Q_OBJECT
public:
    explicit AioPane(QWidget *parent, const QColor &fill = QColor());
protected:
    bool event(QEvent *e) Q_DECL_OVERRIDE;
    void paintEvent(QPaintEvent *e) Q_DECL_OVERRIDE;
private:
    QColor fill;
};

// One line of black text on white, painted by us: setText repaints only this rect and never asks for a relayout.
class AioText : public AioPane {
    Q_OBJECT
public:
    AioText(QWidget *parent, const QFont &face, Qt::Alignment align);
    void setText(const QString &text); // no-op when unchanged
    QString text() const { return str; }
protected:
    void paintEvent(QPaintEvent *e) Q_DECL_OVERRIDE;
private:
    QString str;
    QFont face;
    Qt::Alignment align;
};

// Our own button: icon and/or one line of text, optional pill outline ("chip"). Reports taps from touch events
// or from the mouse events Qt synthesizes out of them; the pressed state is painted (inverted) for 120 ms.
class AioTap : public QWidget {
    Q_OBJECT
public:
    AioTap(QWidget *parent, const QPixmap &icon, const QFont &face, bool chip, bool feedback);
    void setText(const QString &text); // no-op when unchanged
    QString text() const { return str; }
    void setAlignment(Qt::Alignment a) { align = a; }
    void fitWidth(const QStringList &texts, int minWidth); // fixed width for the widest text (set the height first)
Q_SIGNALS:
    void tapped();
protected:
    bool event(QEvent *e) Q_DECL_OVERRIDE;
    void mousePressEvent(QMouseEvent *e) Q_DECL_OVERRIDE;
    void mouseReleaseEvent(QMouseEvent *e) Q_DECL_OVERRIDE;
    void paintEvent(QPaintEvent *e) Q_DECL_OVERRIDE;
private Q_SLOTS:
    void unpress();
private:
    int padding() const;
    int gap() const;
    QRect box() const;
    void press();
    void release(bool inside);
    QPixmap icon, iconInv;
    QString str;
    QFont face;
    Qt::Alignment align;
    bool chip, feedback, down, pressed;
    QTimer *pressTimer;
    QElapsedTimer lastTouch; // mouse events right after a touch are its synthesized copy: ignored
};

// Seek line with a round knob, tap or drag. Opaque; repaints only the band between the old and new knob.
class AioSlider : public QWidget {
    Q_OBJECT
public:
    AioSlider(QWidget *parent, int thick, int thin, const QPixmap &knob);
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
    void moveKnob(int x);
    void press(qreal x);
    void drag(qreal x);
    void release(qreal x);
    qint64 value, maximum, dragValue;
    bool dragging;
    int thick, thin, drawnX;
    QPixmap knob;
    QElapsedTimer lastTouch;
};

// Lives on the application, not on the reader: closing the reader deletes the reader's children, so the
// close call must not run inside one of their slots.
class AioCloser : public QObject {
    Q_OBJECT
public:
    explicit AioCloser(QObject *parent) : QObject(parent) {}
    QPointer<QWidget> reader; // the reader to close (null: already gone, nothing to do)
public Q_SLOTS:
    void close();
};

// One per open .mp3z (child of the MP3Reader widget, so it dies with it).
class AioAudio : public QObject {
    Q_OBJECT
public:
    explicit AioAudio(QWidget *reader);
    ~AioAudio();
    bool owns(QObject *player) const;   // the AudioPlayer belongs to this reader
    void media(QObject *player, int index, const QByteArray &mp3, bool hasData); // one AudioPlayer::setMedia
    void setPlayer(QObject *player);
    void setTrack(int index, const QByteArray &mp3);
    void setCover(const QByteArray &mp3);
    bool playPauseText(QLabel *label, const QString &text); // stock "Play"/"Pause": icon instead of text
    void setChapter(const QString &name);
    QPixmap framedCover(const QPixmap &pm) const;          // a stock cover, fitted into our framed box
    bool eventFilter(QObject *watched, QEvent *e) Q_DECL_OVERRIDE;

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
    void applyPendingLast();
    void applyRate();
    void onDuration();
    void applyPlayState();
    void volumeDown();
    void volumeUp();
    void refreshBluetooth();
    void applyVolumeNow();
    void saveVolumeNow();
    void btTapped();

private:
    int px(int dragon) const;
    int frameWidth() const;
    void seek(qint64 delta);
    void showVolume();
    void stepVolume(int delta);
    void setIcon(QLabel *label, const QPixmap &pm, const QSize &box);
    void updateSleepLabel();
    void updateFooter(qint64 pos, qint64 dur);
    void schedule();
    void requestBluetooth();
    void applyGain();
    void sendVolume();
    void updateBtChip();
    void loadState();
    void saveState(bool askPlayer);
    QString bookKey() const;

    QPointer<QWidget> reader;
    QPointer<QObject> player;
    QPointer<QLabel> cover, playPause;
    QPointer<AioText> titleText, chapterText, elapsedText, remainText, footerText, volText;
    QPointer<AioTap> speedTap, sleepTap, btTap;
    QPointer<AioSlider> slider;
    QTimer *timer = nullptr, *stateTimer = nullptr, *sleepTimer = nullptr;
    QTimer *volApplyTimer = nullptr, *volSaveTimer = nullptr;
    int wantPlaying = -1;
    bool iconSet = false;
    QPixmap playIcon, pauseIcon;
    QSize playBox, coverBox;
    qreal scale = 1;
    int coverKey = 0;
    bool playing = false, started = false, pauseOnPlay = false, footerDirty = true;
    QElapsedTimer footerClock;
    int track = -1, trackNo = 0, trackCount = 0; // track: 0-based key; trackNo/trackCount from ID3 TRCK ("7/15")
    qint64 lastPos = 0, lastDur = 0, pendingSeek = -1, resumeTarget = -1;
    int savedTrack = -1;
    qint64 savedPos = 0;
    QMap<int, qint64> durations;        // learned track lengths, persisted with the position
    QString bookTitle;
    int speedIndex = 0, sleepMode = 0;
    QElapsedTimer sleepClock, skipClock;
    // Bluetooth, read at most twice per reader and always deferred: never on taps, ticks or play/pause changes.
    QString btDev, btName;
    bool btChecked = false, btRequested = false, btRetried = false, btMine = false;
    int sentVolume = -1;                // last level sent to btDev by this reader
};

#endif
