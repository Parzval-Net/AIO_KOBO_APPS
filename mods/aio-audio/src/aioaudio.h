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
#include <QWindow>

class QEvent;
class QGestureEvent;
class QKeyEvent;
class QMouseEvent;
class QPaintEvent;
class QRect;
class QRegion;
class QTimer;

// Passive area (header, rows, bars, cover box): takes every touch and mouse event, so a tap that misses a button
// never falls through to Nickel's reading view (menus, page turns, full refreshes). Optional opaque fill.
// Events over a stock Nickel button (a QLabel/TouchLabel inside the pane) are left alone, so Qt keeps
// propagating them and synthesizes the mouse events that button needs.
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
// or from the mouse events Qt synthesizes out of them; the pressed state is a 3 px outline for 120 ms.
class AioTap : public QWidget {
    Q_OBJECT
public:
    AioTap(QWidget *parent, const QPixmap &icon, const QFont &face, bool chip, bool feedback);
    void setText(const QString &text); // no-op when unchanged
    QString text() const { return str; }
    void setAlignment(Qt::Alignment a) { align = a; }
    void setDetail(const QString &text);  // second text, right-aligned (track number); no-op when unchanged
    void setInverted(bool on);            // drawn white on black (the current track)
    void setInset(int px) { inset = px; } // side padding of plain (non-chip) buttons
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
    QPixmap icon;
    QString str;
    QFont face;
    Qt::Alignment align;
    bool chip, feedback, down, pressed;
    QTimer *pressTimer;
    QString detail;
    bool inverted;
    int inset;
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

// Application event filter, installed only while a restyled player is shown: the page-turn buttons (as Nickel's
// TactileButtonFilter delivers them, already oriented and inverted) seek -30/+30 s instead of changing the track.
class AioKeys : public QObject {
    Q_OBJECT
public:
    explicit AioKeys(QObject *parent) : QObject(parent) {}
    bool eventFilter(QObject *watched, QEvent *e) Q_DECL_OVERRIDE;
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
    QPixmap framedCover(const QPixmap &pm);                // a stock cover, fitted into our framed box (kept)
    bool eventFilter(QObject *watched, QEvent *e) Q_DECL_OVERRIDE;
    QWidget *readerWidget() const { return reader.data(); }
    bool restyled() const { return !rootPane.isNull(); }  // our layout is on (the hooks below may act)
    bool readerShown() const;
    bool trackEnded() const;                               // the player is at the end of its track (auto-advance)
    bool pageKey(QObject *receiver, QKeyEvent *e);         // page-turn button: true when taken
    bool fastRegion(const QRegion &region, QWindow *win); // only our black/white controls changed (reader's window)

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
    void retryBluetooth();
    void applyVolumeNow();
    void saveVolumeNow();
    void savePaused();
    void btTapped();
    void openToc();
    void closeToc();
    void tocPrev();
    void tocNext();
    void tocRowTapped();
    void jumpNow();
    void onPosition(qint64 pos);
    void onState();
    void headsetVolume(int left, bool leftMuted, int right, bool rightMuted); // Device1ProxyFacade::volumeChanged

private:
    int px(int dragon) const;
    int frameWidth() const;
    void seek(qint64 delta);
    void showVolume();
    void stepVolume(int delta);
    void setIcon(QLabel *label, const QPixmap &pm); // the pressed state shows the same icon
    void updateSleepLabel();
    void updateFooter(qint64 pos, qint64 dur);
    void schedule();
    void requestBluetooth();
    bool readBluetooth();               // false: this AioAudio was deleted during a libnickel call
    void applyGain();
    bool sendVolume();                  // false: this AioAudio was deleted during a libnickel call
    void updateBtChip();
    void watchHeadset(QObject *facade); // the headset's own volume changes (Device1ProxyFacade::volumeChanged)
    void loadState();
    void saveState(bool askPlayer);
    QString bookKey() const;
    void installGestureFilters();
    bool gestureOnPane(QGestureEvent *e) const;
    bool isGestureHost(QObject *o) const;
    void setKeyFilter(bool on);
    void relayout();                    // the track list over the reader's real size (Show/Resize)
    void layoutCover();                 // the cover square from its row's real size (row Resize)
    bool decodeCover();                 // coverJpeg at coverBox (scaled JPEG decode), framed; false: nothing decoded
    void redrawCover();
    void resolveBook();                 // the .mp3z path once the Volume is filled: title fallback, track list
    void updateChapterLine();
    void drawPlaceholder();
    bool inFast(const QRect &global) const;
    int currentTrack() const;
    void buildToc();
    void layoutToc();
    void fillToc();
    int stockTrackCount() const;        // QVector<Shortcover> size at MP3Reader+0x2c (validated layout), else 0
    void notePos(qint64 pos);
    void refreshWindow();

    QPointer<QWidget> reader;
    bool layoutOk = false;              // MP3Reader is the 4.45 layout (Volume vtable at +0x24): offsets usable
    QPointer<QObject> player;
    QPointer<QLabel> cover, playPause;
    QPointer<AioPane> rootPane;
    QList<QPointer<QWidget> > gestureHosts; // the reader's ancestors, filtered for Nickel's gestures over our pane
    QPointer<AioText> titleText, chapterText, elapsedText, remainText, footerText, volText;
    QPointer<AioTap> speedTap, sleepTap, btTap;
    QPointer<AioSlider> slider;
    QTimer *timer = nullptr, *stateTimer = nullptr, *sleepTimer = nullptr, *saveTimer = nullptr;
    QTimer *volApplyTimer = nullptr, *volSaveTimer = nullptr;
    QElapsedTimer burstClock, saveClock; // first flip of a play/pause burst; last position save on pause
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
    bool btChecked = false, btRequested = false, btRetried = false, btMine = false, btRetryPending = false;
    int btTries = 0;                    // 1 s retries while another reader's read is in flight (at most 5)
    int sentVolume = -1;                // last level sent to btDev by this reader (or reported by it)
    QElapsedTimer volSendClock;         // last setVolume: the headset's echo of it is not a user change
    QPointer<QObject> btFacade;         // btDev's Device1ProxyFacade (volumeChanged connected)
    // Every row but the cover has a fixed height; the cover row takes the rest (its Resize sizes the cover).
    QPointer<AioPane> coverRow;
    QImage coverSrc;                    // the picture of the cover shown (re-framed when the box changes)
    QByteArray coverJpeg;               // the track's APIC, kept to decode again at a new size (never upscaled)
    bool placeholder = false;
    QString bookPath, fallbackTitle;    // the .mp3z (Content::getId), its folder or file name
    bool pathLogged = false;
    QString chapterName;                // the cleaned track name from the stock title label
    // Fast refresh: these widgets only ever paint black and white.
    QList<QPointer<QWidget> > fastWidgets;
    QPointer<QWindow> readerWin;        // the reader's top-level window (refreshed on Show)
    bool normalFlush = false;           // next flush of that window keeps the default waveform (track change)
    int playerState = -1, prevPlayerState = -1; // QMediaPlayer::State (0 stopped, 1 playing, 2 paused)
    bool keysOn = false;
    QElapsedTimer keyClock;             // auto-repeat of a held page button: one seek per 400 ms
    // "Capitulos": the .mp3z entries as libmp3z orders them, read once on the first open.
    QPointer<AioTap> tocTap, tocPrevTap, tocNextTap, tocClose;
    QPointer<AioPane> tocPane, tocLine;
    QPointer<AioText> tocTitle, tocPageText, tocNote;
    QList<QPointer<AioTap> > tocRows;
    QStringList tocNames;
    bool tocLoaded = false, tocVerified = true; // tocVerified: entry count matches the player's (jumps allowed)
    int tocPage = 0, tocPerPage = 9, tocCurrent = -1, jumpTarget = -1;
};

#endif
