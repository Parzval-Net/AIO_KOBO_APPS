// AIO Audio: Kobo's .mp3z audiobook player (libmp3z.so, firmware 4.x) turned into an Audible/Spotify-style
// player that respects the e-ink hardware.
//
// Stock libmp3z shows a debug screen: English labels ("Backword", "File Name: 001 - x.mp3"), empty
// Forward/Backword slots, linear and log10 volume rows, no elapsed time and, with the GStreamer backend,
// no cover (AudioPlayer::broadcastMetadata only emits trackImageChanged when Qt reports metadata).
//
// Layout, built in our own pane on top of the stock Ui_MP3Reader form (portrait, black on white, sizes scaled
// from a 1680 px tall screen, tap targets >= 106 px). Every row but the cover has a fixed height; the cover is
// the largest square that fits the reader's real height (measured on Show/Resize) and the space left over is
// spread evenly between the groups, so nothing is clipped:
//   header      [<] Mis libros ................................... [toc] ( 1,25x )   112 px, thin black line
//   cover       square, min(78% of the width, 48% of the height), thin black frame, 16 grey levels, read from
//               the track's ID3 APIC (placeholder box with the book title when there is none)
//   title       book title: ID3 TALB, else the .mp3z's folder name, else its file name; bold, one line
//   chapter     cleaned track name, italic, one line
//   scrubber    seek slider (tap or drag); elapsed / "-remaining" 8 px under it, aligned with the line ends
//   transport   [|<]  [-30]  [ play/pause ]  [+30]  [>|]   (stock prev/next chapter buttons keep their wiring)
//   bottom bar  [-] 55% [+]          ( timer 15 min )          [bt] headset name
//   footer      "Pista 3 de 15 · 23% del libro · quedan ≈ 5 h 10 min"
// "Capitulos" (header): full-reader list of the .mp3z tracks (ZIP central directory, sorted like libmp3z), 9 rows
// per page; a tap jumps there through MP3Reader::goToPage and saves the bookmark.
// Extras: sleep timer (15/30/60 min or end of track), playback speed, resume inside the track (positions.ini:
// libmp3z only remembers the track), whole-book progress from TRCK and the learned track lengths.
//
// Track changes come only from our controls. Nickel's reading view turns taps, swipes and page keys on the cover
// or the screen edges into goToNextPage/goToPrevPage signals that reach MP3Reader through its qt_static_metacall;
// while our layout is on, those calls are dropped (unless the track just ended: auto-advance keeps working).
// The stock prev/next buttons (PMF connections) and headset media keys (MP3Reader::keyPressEvent) are not
// affected. The page-turn buttons, as Nickel delivers them (Key_Left/Up = back, Key_Right/Down = forward, already
// oriented and inverted by its TactileButtonFilter), seek -30/+30 s; in the track list they turn its pages.
//
// Performance rules. Kobo Libra Colour (Kaleido colour e-ink): every repainted region goes through kernel colour
// mapping threads that run SCHED_RR and preempt the A2DP encoder and GStreamer, and Nickel usually keeps a single
// core online, so any repaint during playback can make the audio stutter, and any synchronous D-Bus call on the
// GUI thread freezes taps.
//   - BluetoothManager: read once per reader, deferred after the first setMedia (sink path and name). Software
//     volume and unmute at most once per device per Nickel run; setVolume only when the level changed, 300 ms
//     after the last volume tap. Never on taps, ticks or play/pause changes. btVolume is saved 2 s after the
//     last tap. The Bluetooth chip only consumes the tap (one deferred re-read per reader).
//   - Monochrome: icons are pure black/white (no antialiased greys), the cover has 16 grey levels (dithered).
//   - Fast refresh: a flush that only covers our black/white controls (times, slider, buttons in their pressed
//     state, volume, sleep label) asks the Kobo platform plugin for a DU partial update instead of the default
//     colour-aware waveform. Anything else (cover, titles, the track list, whole screens) is left as it was.
//     Create AIO_DIR/no-fast-refresh to turn it off.
//   - Fixed geometry: every row has a fixed height, every changing text a fixed width from its worst case; our
//     texts paint themselves, so a text change repaints its own rect and never relayouts.
//   - The slider is opaque and repaints only the band between the old and the new knob position.
//   - Tap feedback: our buttons paint a pressed state for 120 ms; stock TouchLabels use their own selected
//     pixmap. No stylesheet repolish ever, no feedback on the back link or the cover.
//   - Ticks: every 5 s while playing and visible, only elapsed/remaining and the slider (one band); the footer
//     at most every 60 s or on a track change; the sleep label only when its minute changes. The timer stops
//     when paused or when the reader is hidden; nothing under the track list is repainted while it is open.
//   - Every passive area takes touch and mouse events, so a tap never falls through to the reading view (events
//     over a stock button we kept go on to it), and Nickel's gestures whose hot spot is on our pane are eaten
//     at the reader's ancestors, where its recognizers deliver them.
//   - Nothing is shown or hidden per track: the space is reserved. The track list is parsed once, on its first
//     open, never per tick.
//
// Hooks (PLT entries, verified on 4.45.23697 against each library's .rel.plt: R_ARM_JUMP_SLOT, STB_GLOBAL,
// STT_FUNC), so only calls made by that library are affected. libmp3z.so:
//   MP3Reader::MP3Reader(QWidget*, PluginState*)   called by MP3ReaderPlugin::reader: restyle the Ui_MP3Reader form
//   QLabel::setText                                setupUi, applyVolume, setPage and the play/pause lambda
//   AudioPlayer::setMedia(int, QMediaContent, QIODevice*)  setPage: keep the player, read tags and cover
//   QLabel::setPixmap, QWidget::setGeometry        stock cover lambda: never replaces or moves our cover
//   MP3Reader::goToNextPage/goToPrevPage/goToPage/goToNextChapter/goToPrevChapter   dropped when called from
//                                                  MP3Reader::qt_static_metacall (Nickel's reading view signals)
// libnickel.so: ReadingView::processTap/processSwipe   swallowed while our player is shown in that view
// libQt5Widgets.so.5: QBackingStore::flush(QRegion, QWindow*, QPoint, QList<QPair<QRect, quint64> >)  fast refresh
// Optional symbols: TouchLabel::set(De)SelectedPixmap, ReadingController::closeReadingView, Content::getId,
// BluetoothManager and BluetoothDevice (libnickel, nh_dlsym), _ZTV6Volume (libnickel, dlsym: validates the 4.45
// MP3Reader offsets before any of them is read) and MP3Reader::saveBookmark/goToPage/event/
// staticMetaObject (libmp3z is RTLD_LOCAL: dlopen RTLD_NOLOAD + dlsym at runtime). All hooks and symbols are
// optional: if a firmware changes them, Nickel keeps the stock behaviour for that part.

#include <QBoxLayout>
#include <QBuffer>
#include <QCoreApplication>
#include <QApplication>
#include <QBackingStore>
#include <QDir>
#include <QEvent>
#include <QFile>
#include <QFileInfo>
#include <QFont>
#include <QFontMetrics>
#include <QGesture>
#include <QGuiApplication>
#include <QImage>
#include <QImageReader>
#include <QKeyEvent>
#include <QLabel>
#include <QLayout>
#include <QMetaMethod>
#include <QMetaProperty>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QPen>
#include <QPixmap>
#include <QPair>
#include <QPolygonF>
#include <QRegExp>
#include <QRegion>
#include <QScreen>
#include <QSettings>
#include <QSpacerItem>
#include <QStringList>
#include <QTimer>
#include <QTouchEvent>
#include <QVariant>
#include <QVector>
#include <QWidget>
#include <QWindow>
#include <qmath.h>

#include <dlfcn.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>

#include <NickelHook.h>

#include "aioaudio.h"

#define AIO_DIR "/mnt/onboard/.adds/aio-audio"
#define MP3Z "/usr/local/Kobo/libmp3z.so"
#define MAX_TRACKS 999 // TRCK totals and saved track keys above this are treated as garbage

static void (*MP3Reader_MP3Reader)(void *self, QWidget *parent, void *state);
static void (*QLabel_setText)(QLabel *self, const QString &text);
static void (*QLabel_setPixmap)(QLabel *self, const QPixmap &pixmap);
static void (*QWidget_setGeometry)(QWidget *self, const QRect &rect);
static void (*AudioPlayer_setMedia)(void *self, int index, const void *content, QIODevice *device);
static void (*TouchLabel_setDeselectedPixmap)(QLabel *self, const QPixmap &pixmap); // libnickel, optional
static void (*TouchLabel_setSelectedPixmap)(QLabel *self, const QPixmap &pixmap);   // libnickel, optional
static void (*ReadingController_closeReadingView)();                                 // libnickel, optional
static void (*MP3Reader_saveBookmark)(void *self);                                   // libmp3z, optional
static void (*MP3Reader_goToPageFn)(void *self, int page);                           // libmp3z, 1-based, optional
// Navigation hooks (libmp3z PLT): the originals.
static void (*MP3Reader_goToNextPage)(void *self);
static void (*MP3Reader_goToPrevPage)(void *self);
static void (*MP3Reader_goToPage)(void *self, int page);
static void (*MP3Reader_goToNextChapter)(void *self);
static void (*MP3Reader_goToPrevChapter)(void *self, int how);                       // ReadingNavigationType
// Nickel's reading view (libnickel PLT, optional).
static void (*ReadingView_processTap)(void *self, void *tap);
static void (*ReadingView_processSwipe)(void *self, void *swipe);
// The .mp3z path: Content::getId of the Volume embedded at MP3Reader+0x24 (libnickel, optional; QString sret).
static QString (*Content_getId)(const void *self);
// Kobo's QtGui: the list carries per-rect update flags for the platform plugin (empty: its default waveform).
typedef QList<QPair<QRect, quint64> > AioFlushList;
static void (*QBackingStore_flush)(QBackingStore *self, const QRegion &region, QWindow *win, const QPoint &offset,
                                   const AioFlushList &list);
#define AIO_WAVE_DU_PARTIAL 0x24 // 0x20 DU | 0x4 partial (libkobo)
static bool fastFlushOn = true;  // set false if anything looks wrong: then every flush is left as it was
// MP3Reader::qt_static_metacall, [start, end): calls coming from there are Nickel's reading view signals.
static uintptr_t metacallStart = 0, metacallEnd = 0;
static bool navRangeOk = false;
// Bluetooth output (libnickel, optional). Kobo's own AudiobookPlayer::updateSpeaker pushes a volume to the A2DP
// device before playing; libmp3z never does, so mtkbtd's PCM keeps Volume=0 and the headset stays silent.
static void *(*BluetoothManager_sharedInstance)();
static QString (*BluetoothManager_a2dpSink)(const void *self);                        // "/org/bluez/hci0/dev_..."
static bool (*BluetoothManager_supportsAbsoluteVolume)(void *self, const QString &dev);
static void (*BluetoothManager_setSoftwareVolume)(void *self, const QString &dev, bool software);
static void (*BluetoothManager_setMuted)(void *self, const QString &dev, bool left, bool right);
static void (*BluetoothManager_setVolume)(void *self, const QString &dev, unsigned char left, unsigned char right);
// Headset name (libnickel 4.45, RE): BluetoothDevice is a QPointer to the manager's cached Device1ProxyFacade
// (8 bytes), built from the device path by a lookup in BluetoothManager's map; displayName reads its property.
static void (*BluetoothDevice_BluetoothDevice)(void *self, const QString &path);
static QString (*BluetoothDevice_displayName)(const void *self);
static void (*BluetoothDevice_destroy)(void *self);

static int btVolume = -1;          // 0..127, ours (positions.ini "btVolume"), -1 = not loaded yet
static QStringList btPrepared;     // sinks that already got setSoftwareVolume/setMuted in this Nickel run
static bool btQueued = false;      // one deferred Bluetooth read in flight at a time

// The user partition is mounted (not exported over USB): /mnt/onboard is its own filesystem and our folder,
// which KoboRoot always ships (LEEME.txt), is there. Otherwise a QSettings write would land on the rootfs.
static bool onboardReady() {
    struct stat onboard, mnt;
    if (stat("/mnt/onboard", &onboard) != 0 || stat("/mnt", &mnt) != 0 || onboard.st_dev == mnt.st_dev)
        return false;
    return QDir(QLatin1String(AIO_DIR)).exists();
}

// The headset volume API is complete: only then the volume buttons and the stream gain are ours.
static bool btVolumeApi() {
    return BluetoothManager_sharedInstance && BluetoothManager_a2dpSink && BluetoothManager_setVolume;
}

static int btVolumeLoad() {
    if (btVolume < 0) {
        QSettings s(AIO_DIR "/positions.ini", QSettings::IniFormat);
        btVolume = qBound(0, s.value("btVolume", 80).toInt(), 127);
    }
    return btVolume;
}

static int volumePercent() {
    return (btVolumeLoad() * 100 + 63) / 127;
}

// The headset's display name, or empty. Only from the deferred Bluetooth read.
static QString headsetName(const QString &dev) {
    if (dev.isEmpty() || !BluetoothDevice_BluetoothDevice || !BluetoothDevice_displayName || !BluetoothDevice_destroy)
        return QString();
    void *device[8]; // 8 bytes on 4.45, padded in case a firmware grows it
    memset(device, 0, sizeof device);
    BluetoothDevice_BluetoothDevice(device, dev);
    // 4.45 (RE): the ctor stores {QPointer d, object} or {0, 0} when the device left the manager's map;
    // displayName -> name()/address() check isValid() first. Skip the call (and its warning) when empty.
    QString name;
    if (device[0] && device[1])
        name = BluetoothDevice_displayName(device);
    BluetoothDevice_destroy(device);
    return name.trimmed();
}

// MP3Reader layout on 4.45 (RE): Volume embedded at +0x24 (the constructor runs Volume::Volume on this+0x24,
// which stores &_ZTV6Volume + 8 as its first word), QVector<Shortcover> at +0x2c, current index at +0x30,
// AudioPlayer at +0x40. Those offsets are only used when that first word is libnickel's Volume vtable: any other
// build fails the check and keeps the stock-safe paths (no track list, no path, no offset reads).
static bool readerLayout(const QWidget *reader) {
    static const void *volumeVptr = 0;
    static bool resolved = false;
    if (!resolved) {
        resolved = true;
        void *vt = dlsym(RTLD_DEFAULT, "_ZTV6Volume");
        if (!vt) {
            if (void *h = dlopen("libnickel.so.1.0.0", RTLD_LAZY | RTLD_NOLOAD)) {
                vt = dlsym(h, "_ZTV6Volume");
                dlclose(h); // only drops the reference NOLOAD added
            }
        }
        if (vt)
            volumeVptr = static_cast<const char *>(vt) + 8; // vtable group: offset-to-top, typeinfo, then the vptr
    }
    if (!reader || !volumeVptr)
        return false;
    return *reinterpret_cast<const void *const *>(reinterpret_cast<const char *>(reader) + 0x24) == volumeVptr;
}

// The AudioPlayer of an MP3Reader (MP3Reader+0x40 on 4.45, RE). Only with a validated layout.
static QObject *readerPlayer(const QWidget *reader) {
    return *reinterpret_cast<QObject *const *>(reinterpret_cast<const char *>(reader) + 0x40);
}

// The .mp3z of an MP3Reader: Content::getId of its Volume (embedded at MP3Reader+0x24 on 4.45, RE: setPage and
// saveBookmark pass this+0x24), "file://" stripped. Empty unless it looks like an absolute .mp3z path. Only with a
// validated layout.
static QString readerPath(const QWidget *reader) {
    if (!Content_getId || !reader)
        return QString();
    QString id = Content_getId(reinterpret_cast<const char *>(reader) + 0x24);
    if (id.startsWith(QLatin1String("file://")))
        id = id.mid(7);
    if (!id.startsWith(QLatin1String("/")) || !id.endsWith(QLatin1String(".mp3z"), Qt::CaseInsensitive))
        return QString();
    return id;
}

static quint32 le16(const unsigned char *b) {
    return quint32(b[0]) | quint32(b[1]) << 8;
}
static quint32 le32(const unsigned char *b) {
    return quint32(b[0]) | quint32(b[1]) << 8 | quint32(b[2]) << 16 | quint32(b[3]) << 24;
}

// The entry names of a ZIP (.mp3z) as libmp3z orders its tracks (4.45, RE): every entry, UTF-8,
// QStringList::sort(). Only the End Of Central Directory (last 66 KB) and the central directory are read; every
// field is checked against what was read. Every entry is read (5000 at most), sorted, and more than MAX_TRACKS
// means no list: a truncated list would not match libmp3z's order. Empty on any doubt (ZIP64, damaged file).
static QStringList zipNames(const QString &path) {
    QStringList names;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return names;
    const qint64 size = f.size();
    if (size < 22)
        return names;
    const qint64 tailLen = qMin<qint64>(size, 66 * 1024);
    if (!f.seek(size - tailLen))
        return names;
    const QByteArray tail = f.read(tailLen);
    if (qint64(tail.size()) != tailLen)
        return names;
    const unsigned char *t = reinterpret_cast<const unsigned char *>(tail.constData());
    int e = -1;
    for (int i = tail.size() - 22; i >= 0; i--)
        if (t[i] == 'P' && t[i + 1] == 'K' && t[i + 2] == 5 && t[i + 3] == 6) {
            e = i;
            break;
        }
    if (e < 0)
        return names;
    const quint32 entries = le16(t + e + 10), cdSize = le32(t + e + 12), cdOff = le32(t + e + 16);
    const qint64 eocdPos = size - tailLen + e;
    if (entries == 0xffff || cdSize == 0xffffffffu || cdOff == 0xffffffffu)
        return names; // ZIP64
    if (cdSize < 46 || cdSize > 8u * 1024 * 1024 || qint64(cdOff) + qint64(cdSize) > eocdPos || !f.seek(cdOff))
        return names;
    const QByteArray cd = f.read(cdSize);
    if (quint32(cd.size()) != cdSize)
        return names;
    const unsigned char *c = reinterpret_cast<const unsigned char *>(cd.constData());
    quint32 pos = 0;
    while (cdSize - pos >= 46 && names.size() < 5000 && quint32(names.size()) < entries) {
        if (le32(c + pos) != 0x02014b50u)
            break;
        const quint32 nl = le16(c + pos + 28), xl = le16(c + pos + 30), cl = le16(c + pos + 32);
        if (nl > cdSize - pos - 46)
            break;
        names << QString::fromUtf8(reinterpret_cast<const char *>(c + pos + 46), int(nl));
        const quint32 step = 46 + nl + xl + cl;
        if (step > cdSize - pos)
            break;
        pos += step;
    }
    names.sort();
    if (names.size() > MAX_TRACKS)
        return QStringList();
    return names;
}

static QPointer<AioAudio> aio;
static QPointer<AioKeys> keyFilter;

// The last setMedia no AioAudio took (setPage inside the MP3Reader constructor, before its AioAudio existed).
// Kept only while a reader is being constructed: nobody else would replay it.
static QPointer<QObject> lastMediaPlayer;
static int lastMediaIndex = -1;
static QByteArray lastMediaData;
static bool lastMediaHasData = false;
static bool inReaderCtor = false;

static void forgetMedia() {
    lastMediaPlayer = 0;
    lastMediaIndex = -1;
    lastMediaData = QByteArray(); // the whole track: let it go
    lastMediaHasData = false;
}

#define RATE_COUNT 6
static const qreal RATES[RATE_COUNT] = {1.0, 1.25, 1.5, 1.75, 2.0, 0.8};
static const char *const RATE_NAMES[RATE_COUNT] = {"1x", "1,25x", "1,5x", "1,75x", "2x", "0,8x"};
static const int SLEEP_MIN[] = {0, 15, 30, 60};

// libmp3z is loaded RTLD_LOCAL by Nickel's plugin loader, so RTLD_DEFAULT may not see its symbols.
static void resolveMp3z() {
    static bool done = false;
    if (done)
        return;
    done = true;
    static const char sym[] = "_ZN9MP3Reader12saveBookmarkEv";
    static const char page[] = "_ZN9MP3Reader8goToPageEi";
    void *smo = 0, *ev = 0;
    if (void *h = dlopen(MP3Z, RTLD_LAZY | RTLD_NOLOAD)) {
        MP3Reader_saveBookmark = reinterpret_cast<void (*)(void *)>(dlsym(h, sym));
        MP3Reader_goToPageFn = reinterpret_cast<void (*)(void *, int)>(dlsym(h, page)); // the function, not its PLT
        smo = dlsym(h, "_ZN9MP3Reader16staticMetaObjectE");
        ev = dlsym(h, "_ZN9MP3Reader5eventEP6QEvent");
        dlclose(h); // only drops the reference NOLOAD added: the plugin keeps the library loaded
    }
    if (!MP3Reader_saveBookmark)
        MP3Reader_saveBookmark = reinterpret_cast<void (*)(void *)>(dlsym(RTLD_DEFAULT, sym));
    if (!MP3Reader_goToPageFn)
        MP3Reader_goToPageFn = reinterpret_cast<void (*)(void *, int)>(dlsym(RTLD_DEFAULT, page));
    if (!MP3Reader_saveBookmark)
        nh_log("MP3Reader::saveBookmark not found, closing without it");
    if (!MP3Reader_goToPageFn)
        nh_log("AIO Audio: MP3Reader::goToPage not found, no track list");
    // qt_static_metacall is not exported: its address is in the class's QMetaObject (d.static_metacall); the
    // function that follows it in libmp3z 4.45 is MP3Reader::event. Anything unexpected: nothing is blocked.
    if (smo && ev) {
        const QMetaObject *mo = static_cast<const QMetaObject *>(smo);
        const uintptr_t start = reinterpret_cast<uintptr_t>(mo->d.static_metacall) & ~uintptr_t(1);
        const uintptr_t end = reinterpret_cast<uintptr_t>(ev) & ~uintptr_t(1);
        if (start && start < end && end < start + 0x2000) {
            metacallStart = start;
            metacallEnd = end;
            navRangeOk = true;
        }
    }
    if (!navRangeOk)
        nh_log("AIO Audio: MP3Reader::qt_static_metacall range unknown, Nickel's page turns are left alone");
}

template <class T> static T *child(QWidget *root, const char *name) {
    return root->findChild<T *>(QLatin1String(name));
}

// Take a widget out of whatever stock layout holds it (removeWidget is a no-op elsewhere).
static void detach(QWidget *root, QWidget *w) {
    if (!w)
        return;
    QList<QLayout *> ls = root->findChildren<QLayout *>();
    for (int i = 0; i < ls.size(); i++)
        ls.at(i)->removeWidget(w);
}

// What is left of the stock form: widgets hidden, spacers and margins to zero (our pane takes the whole reader).
static void collapse(QLayout *l) {
    l->setContentsMargins(0, 0, 0, 0);
    l->setSpacing(0);
    for (int i = 0; i < l->count(); i++) {
        QLayoutItem *it = l->itemAt(i);
        if (!it)
            continue;
        if (QWidget *w = it->widget())
            w->hide();
        else if (QSpacerItem *sp = it->spacerItem())
            sp->changeSize(0, 0, QSizePolicy::Fixed, QSizePolicy::Fixed);
        else if (QLayout *sub = it->layout())
            collapse(sub);
    }
    l->invalidate();
}

static QFont makeFont(const char *family, int pixels, bool bold, bool italic = false) {
    QFont f(QString::fromLatin1(family));
    f.setPixelSize(qMax(6, pixels));
    f.setBold(bold);
    f.setItalic(italic);
    return f;
}

// No antialiasing: pure black glyphs, so a DU update leaves no grey residue (the fast-refresh widgets).
static QFont crisp(QFont f) {
    f.setStyleStrategy(QFont::NoAntialias);
    return f;
}

static int textWidth(const QFont &f, const QString &s) {
    return QFontMetrics(f).width(s);
}

// Worst case of "10:00:00" (minus: "-10:00:00") in this font, digits not assumed tabular.
static int clockWidth(const QFont &f, bool minus) {
    QFontMetrics fm(f);
    int digit = 0;
    for (char c = '0'; c <= '9'; c++)
        digit = qMax(digit, fm.width(QChar(c)));
    return 6 * digit + 2 * fm.width(QChar(':')) + (minus ? fm.width(QChar('-')) : 0);
}

static QString hms(qint64 ms) {
    qint64 s = ms > 0 ? ms / 1000 : 0;
    if (s >= 3600)
        return QString("%1:%2:%3").arg(s / 3600).arg(s / 60 % 60, 2, 10, QChar('0')).arg(s % 60, 2, 10, QChar('0'));
    return QString("%1:%2").arg(s / 60).arg(s % 60, 2, 10, QChar('0'));
}

static QString span(qint64 ms) {
    qint64 min = (qMax<qint64>(0, ms) + 59999) / 60000;
    if (min >= 60)
        return QString("%1 h %2 min").arg(min / 60).arg(min % 60);
    return QString("%1 min").arg(min);
}

// Names that only number a part ("Parte 1", "CD 2"), as Kobo AIO's audiobooks.py _PART_DIR.
static bool isPartName(const QString &s) {
    static const QRegExp re(QLatin1String("^(parte?|part|cd|dis[ck]o?|volumen|vol\\.?|tomo)\\s*\\d+$"), Qt::CaseInsensitive);
    return re.exactMatch(s);
}

// "007 - Capitulo 3 (2-4).mp3" -> "Capitulo 3 · parte 2 de 4", "Parte 1 (3-15)" -> "Parte 1 · 3/15", "(1-1)" is
// dropped (entries made by Kobo AIO: "NNN - chapter (part-parts).mp3"; others are cleaned up).
static QString cleanTrackName(QString t) {
    t = t.trimmed();
    int dot = t.lastIndexOf('.');
    if (dot > 0 && t.length() - dot <= 5)
        t.truncate(dot);
    int i = 0;
    while (i < t.length() && t.at(i).isDigit())
        i++;
    if (i > 0 && i < t.length()) {
        QString rest = t.mid(i);
        while (!rest.isEmpty() && (rest.at(0) == ' ' || rest.at(0) == '-' || rest.at(0) == '_' || rest.at(0) == '.'))
            rest.remove(0, 1);
        if (!rest.isEmpty())
            t = rest;
    }
    t.replace('_', ' ');
    if (t.endsWith(')')) {
        int open = t.lastIndexOf('(');
        QStringList kn = t.mid(open + 1, t.length() - open - 2).split('-');
        bool ok1 = false, ok2 = false;
        if (open > 0 && kn.size() == 2) {
            int k = kn[0].toInt(&ok1), n = kn[1].toInt(&ok2);
            if (ok1 && ok2 && k > 0 && n >= k) {
                QString name = t.left(open).trimmed();
                if (n == 1)
                    t = name; // a single part: nothing to number
                else // one arg() pass: no %N from the name
                    t = QString::fromUtf8(isPartName(name) ? "%1 · %2/%3" : "%1 · parte %2 de %3")
                            .arg(name, QString::number(k), QString::number(n));
            }
        }
    }
    return t;
}

// Stock title label: "File Name: 007 - x.mp3".
static QString trackTitle(const QString &t) {
    return cleanTrackName(t.mid(11));
}

// "a · a · b" -> "a · b" (a chapter named like its part).
static QString dedupe(const QString &name) {
    const QString sep = QString::fromUtf8(" · ");
    const QStringList parts = name.split(sep);
    QStringList out;
    for (int i = 0; i < parts.size(); i++) {
        const QString p = parts.at(i).trimmed();
        if (p.isEmpty() || (!out.isEmpty() && out.last().compare(p, Qt::CaseInsensitive) == 0))
            continue;
        out << p;
    }
    return out.join(sep);
}

static QString translate(QLabel *label, const QString &text, bool compact) {
    const QString name = label->objectName();
    if (name == "playPause") {
        if (text == "Play")  return QString::fromUtf8("Reproducir");
        if (text == "Pause") return QString::fromUtf8("Pausa");
        return text;
    }
    if (name == "title")
        return text.startsWith("File Name: ") ? trackTitle(text) : text;
    if (name == "volume") {
        if (text.startsWith("Volume: "))
            return compact ? text.mid(8) : QString::fromUtf8("Volumen ") + text.mid(8);
        return text;
    }
    static const struct { const char *name, *text; } fixed[] = {
        {"backward", "« 30 s"}, {"forward", "30 s »"},
        {"slower", "0,8×"}, {"normal", "1×"}, {"faster", "1,2×"},
        {"prevChapter", "« Anterior"}, {"nextChapter", "Siguiente »"},
        {"logVolumeDown", "Volumen −"}, {"logVolumeUp", "Volumen +"},
        {"volumeDown", "Volumen −"}, {"volumeUp", "Volumen +"},
        {"total", ""}, {"current", ""},
    };
    for (const auto &f : fixed)
        if (name == QLatin1String(f.name))
            return QString::fromUtf8(f.text);
    return text;
}

// Connect a Nickel TouchLabel's tapped(...) signal (found through its metaobject, so no libnickel headers).
static bool onTap(QObject *button, QObject *receiver, const char *slot) {
    if (!button)
        return false;
    const QMetaObject *mo = button->metaObject();
    int best = -1;
    for (int i = 0; i < mo->methodCount(); i++) {
        QMetaMethod m = mo->method(i);
        if (m.methodType() == QMetaMethod::Signal && m.name() == "tapped" &&
            (best < 0 || m.parameterCount() < mo->method(best).parameterCount()))
            best = i;
    }
    int s = receiver->metaObject()->indexOfSlot(slot);
    if (best < 0 || s < 0)
        return false;
    return QObject::connect(button, mo->method(best), receiver, receiver->metaObject()->method(s));
}

// -- ID3v2 ------------------------------------------------------------------------------------------------

static quint32 be32(const unsigned char *b) {
    return quint32(b[0]) << 24 | quint32(b[1]) << 16 | quint32(b[2]) << 8 | quint32(b[3]);
}
static quint32 syncsafe(const unsigned char *b) {
    return quint32(b[0] & 0x7f) << 21 | quint32(b[1] & 0x7f) << 14 | quint32(b[2] & 0x7f) << 7 | quint32(b[3] & 0x7f);
}

// The payload of the first `id` frame of an ID3v2.3/2.4 tag (no copy: valid while `mp3` is). Every size is
// checked against what is left, so a damaged tag only means "no frame".
static QByteArray id3Frame(const QByteArray &mp3, const char *id) {
    const unsigned char *d = reinterpret_cast<const unsigned char *>(mp3.constData());
    const quint32 len = quint32(mp3.size());
    if (len < 10 || memcmp(d, "ID3", 3) || d[3] < 3 || d[3] > 4 || (d[5] & 0x80)) // unsynchronised tags: skip
        return QByteArray();
    const bool v4 = d[3] == 4;
    quint32 end = qMin(len, 10 + syncsafe(d + 6));
    quint32 p = 10;
    if (d[5] & 0x40) { // extended header
        if (end - p < 4)
            return QByteArray();
        quint32 ext = v4 ? syncsafe(d + p) : be32(d + p);
        if (!v4 && ext <= 0xfffffff0u)
            ext += 4; // v2.3 does not count its own size field
        if (ext > end - p)
            return QByteArray();
        p += ext;
    }
    while (end - p >= 10 && d[p]) {
        quint32 size = v4 ? syncsafe(d + p + 4) : be32(d + p + 4);
        if (size == 0 || size > end - p - 10)
            break;
        if (!memcmp(d + p, id, 4))
            return QByteArray::fromRawData(reinterpret_cast<const char *>(d + p + 10), int(size));
        p += 10 + size;
    }
    return QByteArray();
}

// The APIC picture data.
static QByteArray apic(const QByteArray &mp3) {
    const QByteArray fr = id3Frame(mp3, "APIC");
    const quint32 n = quint32(fr.size());
    if (n < 1)
        return QByteArray();
    const unsigned char *f = reinterpret_cast<const unsigned char *>(fr.constData());
    quint32 i = 1, enc = f[0];
    while (i < n && f[i]) i++;          // MIME type
    i += 2;                              // NUL + picture type
    if (enc == 1 || enc == 2) {          // UTF-16 description, 2-byte NUL
        while (i + 1 < n && (f[i] || f[i + 1])) i += 2;
        i += 2;
    } else {
        while (i < n && f[i]) i++;
        i += 1;
    }
    if (i < n)
        return QByteArray(reinterpret_cast<const char *>(f + i), int(n - i));
    return QByteArray();
}

// A text frame (TALB, TRCK, ...): Latin-1, UTF-16 with or without BOM, or UTF-8.
static QString id3Text(const QByteArray &mp3, const char *id) {
    const QByteArray fr = id3Frame(mp3, id);
    const int n = fr.size();
    if (n < 2)
        return QString();
    const unsigned char *f = reinterpret_cast<const unsigned char *>(fr.constData());
    const unsigned char enc = f[0];
    QString out;
    if (enc == 1 || enc == 2) {
        int i = 1;
        bool le = false;
        if (enc == 1 && n >= 3) {
            if (f[1] == 0xff && f[2] == 0xfe) { le = true; i = 3; }
            else if (f[1] == 0xfe && f[2] == 0xff) i = 3;
        }
        for (; i + 1 < n; i += 2) {
            ushort c = le ? ushort(f[i] | (f[i + 1] << 8)) : ushort((f[i] << 8) | f[i + 1]);
            if (!c)
                break;
            out.append(QChar(c));
        }
    } else {
        int e = 1;
        while (e < n && f[e])
            e++;
        const char *b = reinterpret_cast<const char *>(f + 1);
        out = enc == 3 ? QString::fromUtf8(b, e - 1) : QString::fromLatin1(b, e - 1);
    }
    return out.trimmed();
}


// -- icons ------------------------------------------------------------------------------------------------

enum Glyph { G_PLAY, G_PAUSE, G_BACK30, G_FWD30, G_CHEVRON, G_BT, G_TIMER, G_KNOB, G_SKIP_PREV, G_SKIP_NEXT, G_MINUS, G_PLUS, G_TOC };

// Painted stand-ins, pure black on transparent (no antialiasing: crisp on e-ink and safe for DU updates), for
// when a Nickel resource is missing.
static QPixmap glyph(Glyph g, int size) {
    size = qMax(8, size);
    QImage img(size, size, QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::transparent);
    const qreal s = size, lw = qMax<qreal>(2, s / 14);
    {
        QPainter p(&img);
        p.setPen(QPen(Qt::black, lw, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.setBrush(Qt::NoBrush);
        auto P = [s](qreal x, qreal y) { return QPointF(x * s, y * s); };
        const QRectF ring(lw, lw, s - 2 * lw, s - 2 * lw);
        switch (g) {
        case G_PLAY: {
            p.drawEllipse(ring);
            QPolygonF tri;
            tri << P(0.40, 0.30) << P(0.72, 0.50) << P(0.40, 0.70);
            p.setBrush(Qt::black);
            p.drawPolygon(tri);
            break;
        }
        case G_PAUSE:
            p.drawEllipse(ring);
            p.fillRect(QRectF(P(0.36, 0.30), P(0.46, 0.70)), Qt::black);
            p.fillRect(QRectF(P(0.54, 0.30), P(0.64, 0.70)), Qt::black);
            break;
        case G_BACK30:
        case G_FWD30: { // counter-clockwise ring with the arrow head at the top right (mirrored below for +30)
            const qreal r = s / 2 - lw * 1.5, a = 70 * M_PI / 180;
            const QPointF c(s / 2, s / 2);
            p.drawArc(QRectF(c.x() - r, c.y() - r, 2 * r, 2 * r), 110 * 16, 320 * 16);
            const QPointF tip(c.x() + r * qCos(a), c.y() - r * qSin(a));
            const QPointF dir(-qSin(a), -qCos(a)), nrm(-dir.y(), dir.x());
            const qreal hl = s * 0.16;
            QPolygonF head;
            head << tip + dir * hl << tip + nrm * (hl * 0.6) << tip - nrm * (hl * 0.6);
            p.setPen(Qt::NoPen);
            p.setBrush(Qt::black);
            p.drawPolygon(head);
            break;
        }
        case G_CHEVRON: {
            QPolygonF c;
            c << P(0.62, 0.20) << P(0.32, 0.50) << P(0.62, 0.80);
            p.drawPolyline(c);
            break;
        }
        case G_BT: {
            QPolygonF b;
            b << P(0.32, 0.32) << P(0.68, 0.66) << P(0.50, 0.84) << P(0.50, 0.16) << P(0.68, 0.34) << P(0.32, 0.68);
            p.drawPolyline(b);
            break;
        }
        case G_TIMER:
            p.drawEllipse(QRectF(P(0.16, 0.22), P(0.84, 0.90)));
            p.drawLine(P(0.50, 0.56), P(0.50, 0.36));
            p.drawLine(P(0.50, 0.56), P(0.64, 0.56));
            p.drawLine(P(0.40, 0.08), P(0.60, 0.08));
            p.drawLine(P(0.50, 0.08), P(0.50, 0.22));
            break;
        case G_KNOB:
            p.setPen(Qt::NoPen);
            p.setBrush(Qt::black);
            p.drawEllipse(QRectF(P(0.10, 0.10), P(0.90, 0.90)));
            break;
        case G_SKIP_PREV:
        case G_SKIP_NEXT: { // bar + triangle (|<), mirrored below for >|
            p.setPen(Qt::NoPen);
            p.setBrush(Qt::black);
            p.drawRect(QRectF(P(0.20, 0.24), P(0.30, 0.76)));
            QPolygonF tri;
            tri << P(0.80, 0.24) << P(0.34, 0.50) << P(0.80, 0.76);
            p.drawPolygon(tri);
            break;
        }
        case G_MINUS:
            p.drawLine(P(0.24, 0.50), P(0.76, 0.50));
            break;
        case G_PLUS:
            p.drawLine(P(0.24, 0.50), P(0.76, 0.50));
            p.drawLine(P(0.50, 0.24), P(0.50, 0.76));
            break;
        case G_TOC: // three list lines with their bullets
            for (int i = 0; i < 3; i++) {
                const qreal y = 0.30 + i * 0.20;
                p.fillRect(QRectF(P(0.14, y - 0.045), P(0.24, y + 0.045)), Qt::black);
                p.drawLine(P(0.34, y), P(0.84, y));
            }
            break;
        }
    }
    if (g == G_FWD30 || g == G_SKIP_NEXT)
        img = img.mirrored(true, false);
    if (g == G_BACK30 || g == G_FWD30) {
        QPainter p(&img);
        p.setRenderHint(QPainter::TextAntialiasing);
        QFont f;
        f.setPixelSize(qMax(6, int(s * 0.30)));
        f.setBold(true);
        p.setFont(f);
        p.setPen(Qt::black);
        p.drawText(QRectF(0, 0, s, s), Qt::AlignCenter, QString("30"));
    }
    return QPixmap::fromImage(img);
}

// Pure black and white: dark opaque pixels black, light opaque pixels white, the rest transparent.
static QPixmap monochrome(const QPixmap &pm) {
    QImage img = pm.toImage().convertToFormat(QImage::Format_ARGB32);
    for (int y = 0; y < img.height(); y++) {
        QRgb *line = reinterpret_cast<QRgb *>(img.scanLine(y));
        for (int x = 0; x < img.width(); x++) {
            const QRgb c = line[x];
            if (qAlpha(c) < 128)
                line[x] = 0;
            else
                line[x] = qGray(c) < 128 ? 0xff000000u : 0xffffffffu;
        }
    }
    return QPixmap::fromImage(img);
}

// A Nickel resource at `size` px tall (Dragon sizes scaled to the screen), in black and white, or the painted
// stand-in.
static QPixmap nickelIcon(const char *path, int size, Glyph fallback) {
    QPixmap pm(QString::fromLatin1(path));
    if (pm.isNull() || pm.height() <= 0)
        return glyph(fallback, size);
    if (qAbs(pm.height() - size) * 10 > size)
        pm = pm.scaledToHeight(size, Qt::SmoothTransformation);
    return monochrome(pm);
}

// 16 grey levels, ordered dither: what the e-ink shows well, and far cheaper to map than full colour.
static QImage grey16(const QImage &src) {
    QImage img = src.convertToFormat(QImage::Format_RGB32);
    for (int y = 0; y < img.height(); y++) {
        QRgb *line = reinterpret_cast<QRgb *>(img.scanLine(y));
        for (int x = 0; x < img.width(); x++) {
            const int g = qGray(line[x]);
            line[x] = qRgb(g, g, g);
        }
    }
    static QVector<QRgb> greys;
    if (greys.isEmpty())
        for (int i = 0; i < 16; i++)
            greys << qRgb(i * 17, i * 17, i * 17);
    return img.convertToFormat(QImage::Format_Indexed8, greys, Qt::OrderedDither);
}

// Pressed state: the icon in white on a black box.
static QPixmap inverted(const QPixmap &pm, const QSize &box) {
    QSize sz = box.expandedTo(pm.size());
    QImage img(sz, QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::white);
    {
        QPainter p(&img);
        p.drawPixmap((sz.width() - pm.width()) / 2, (sz.height() - pm.height()) / 2, pm);
    }
    img.invertPixels();
    return QPixmap::fromImage(img);
}

// Thin black frame of width lw around `r` (inside it).
static void drawFrame(QPainter &p, const QRect &r, int lw) {
    p.fillRect(QRect(r.left(), r.top(), r.width(), lw), Qt::black);
    p.fillRect(QRect(r.left(), r.bottom() - lw + 1, r.width(), lw), Qt::black);
    p.fillRect(QRect(r.left(), r.top(), lw, r.height()), Qt::black);
    p.fillRect(QRect(r.right() - lw + 1, r.top(), lw, r.height()), Qt::black);
}

// A picture fitted into the square `box` (white), with a thin black frame hugging the picture.
static QPixmap framed(const QImage &src, const QSize &box, int lw) {
    QPixmap out(box);
    out.fill(Qt::white);
    if (src.isNull())
        return out;
    const QSize inner(qMax(1, box.width() - 2 * lw), qMax(1, box.height() - 2 * lw));
    QImage img = src;
    if (img.width() > inner.width() || img.height() > inner.height())
        img = img.scaled(inner, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    img = grey16(img);
    const int w = img.width() + 2 * lw, h = img.height() + 2 * lw;
    const QRect r((box.width() - w) / 2, (box.height() - h) / 2, w, h);
    QPainter p(&out);
    p.drawImage(r.left() + lw, r.top() + lw, img);
    drawFrame(p, r, lw);
    return out;
}

// A mouse event this soon after a touch is the copy the platform plugin may also deliver.
static bool recentTouch(const QElapsedTimer &t) {
    return t.isValid() && t.elapsed() < 500;
}

// -- AioPane / AioText ------------------------------------------------------------------------------------

AioPane::AioPane(QWidget *parent, const QColor &f) : QWidget(parent), fill(f) {
    setAttribute(Qt::WA_AcceptTouchEvents);
    if (fill.isValid())
        setAttribute(Qt::WA_OpaquePaintEvent);
}

// The deepest widget at `p` (pane coordinates), or one of its ancestors below the pane, is a stock Nickel button
// we kept (a QLabel/TouchLabel marked aioOwned: play/pause, +-30, prev/next, stock volume): its events must keep
// propagating, or Qt never synthesizes the mouse events it needs. Stock labels that are not buttons (the cover,
// the stock volume text) stay swallowed like any passive area.
static bool stockButtonAt(QWidget *pane, const QPoint &p) {
    for (QWidget *w = pane->childAt(p); w && w != pane; w = w->parentWidget())
        if (!qobject_cast<AioPane *>(w) && !qobject_cast<AioTap *>(w) && !qobject_cast<AioSlider *>(w) &&
            w->property("aioOwned").toBool()) // AioText is an AioPane
            return true;
    return false;
}

bool AioPane::event(QEvent *e) {
    switch (e->type()) {
    case QEvent::TouchBegin: {
        // Qt delivers TouchBegin to the deepest widget with WA_AcceptTouchEvents and maps the points to each
        // widget it propagates to, so pos() is in our coordinates. Over a stock button: not ours.
        const QTouchEvent *te = static_cast<QTouchEvent *>(e);
        if (!te->touchPoints().isEmpty() && stockButtonAt(this, te->touchPoints().first().pos().toPoint())) {
            e->ignore();
            return false;
        }
        e->accept();
        return true;
    }
    case QEvent::MouseButtonPress:
    case QEvent::MouseButtonRelease:
    case QEvent::MouseButtonDblClick:
    case QEvent::MouseMove:
        // positions are mapped to us as the event propagates from the widget under the point
        if (stockButtonAt(this, static_cast<QMouseEvent *>(e)->pos())) {
            e->ignore();
            return false;
        }
        e->accept();
        return true;
    case QEvent::TouchUpdate:
    case QEvent::TouchEnd:
    case QEvent::TouchCancel:
    case QEvent::Gesture:
    case QEvent::GestureOverride:
        e->accept(); // ours (we accepted its TouchBegin): nothing below the player reacts to it
        return true;
    default:
        return QWidget::event(e);
    }
}

void AioPane::paintEvent(QPaintEvent *e) {
    if (!fill.isValid())
        return;
    QPainter p(this);
    p.fillRect(e->rect(), fill);
}

AioText::AioText(QWidget *parent, const QFont &f, Qt::Alignment a) : AioPane(parent, Qt::white), face(f), align(a) {
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
}

void AioText::setText(const QString &t) {
    if (t == str)
        return;
    str = t;
    update(); // this rect only: the geometry is fixed, nothing is relaid out
}

void AioText::paintEvent(QPaintEvent *e) {
    QPainter p(this);
    p.fillRect(e->rect(), Qt::white);
    if (str.isEmpty())
        return;
    p.setFont(face);
    p.setPen(Qt::black);
    p.drawText(rect(), align | Qt::AlignVCenter, QFontMetrics(face).elidedText(str, Qt::ElideRight, width()));
}

// -- AioTap -----------------------------------------------------------------------------------------------

AioTap::AioTap(QWidget *parent, const QPixmap &i, const QFont &f, bool c, bool fb)
    : QWidget(parent), icon(i), face(f), align(Qt::AlignHCenter), chip(c), feedback(fb), down(false), pressed(false),
      pressTimer(new QTimer(this)), inverted(false), inset(0) {
    setAttribute(Qt::WA_AcceptTouchEvents);
    setAttribute(Qt::WA_OpaquePaintEvent);
    if (!icon.isNull() && feedback)
        iconInv = ::inverted(icon, icon.size());
    pressTimer->setSingleShot(true);
    pressTimer->setInterval(120);
    connect(pressTimer, SIGNAL(timeout()), this, SLOT(unpress()));
}

void AioTap::setText(const QString &t) {
    if (t == str)
        return;
    str = t;
    update();
}

void AioTap::setDetail(const QString &t) {
    if (t == detail)
        return;
    detail = t;
    update();
}

void AioTap::setInverted(bool on) {
    if (on == inverted)
        return;
    inverted = on;
    update();
}

int AioTap::padding() const {
    return chip ? height() / 4 : inset;
}

int AioTap::gap() const {
    return qMax(4, height() / 10);
}

// The drawn chip (a pill inside the tap target) or, for plain buttons, the whole target.
QRect AioTap::box() const {
    if (!chip)
        return rect();
    const int inset = height() / 5;
    return rect().adjusted(1, inset, -1, -inset);
}

void AioTap::fitWidth(const QStringList &texts, int minWidth) {
    QFontMetrics fm(face);
    int tw = 0;
    for (int i = 0; i < texts.size(); i++)
        tw = qMax(tw, fm.width(texts.at(i)));
    const int w = 2 * padding() + icon.width() + (tw > 0 && !icon.isNull() ? gap() : 0) + tw + 4;
    setFixedWidth(qMax(minWidth, w));
}

// Pure black and white (no antialiased outline): the pressed state and the inverted state are white on black.
void AioTap::paintEvent(QPaintEvent *) {
    QPainter p(this);
    const bool dark = pressed != inverted;
    p.fillRect(rect(), Qt::white);
    const QRect b = box();
    if (chip) {
        const int lw = qMax(2, height() / 48);
        const qreal radius = b.height() / 2.0;
        p.setPen(dark ? QPen(Qt::NoPen) : QPen(Qt::black, lw));
        p.setBrush(dark ? QBrush(Qt::black) : QBrush(Qt::NoBrush));
        p.drawRoundedRect(QRectF(b).adjusted(lw / 2.0, lw / 2.0, -lw / 2.0, -lw / 2.0), radius, radius);
    } else if (dark) {
        p.fillRect(b, Qt::black);
    }
    QFontMetrics fm(face);
    const int pad = padding();
    const int g = !icon.isNull() && !str.isEmpty() ? gap() : 0;
    const int dw = detail.isEmpty() ? 0 : fm.width(detail) + 2 * gap();
    const QString t = fm.elidedText(str, Qt::ElideRight, qMax(0, b.width() - 2 * pad - icon.width() - g - dw));
    const int cw = icon.width() + g + (t.isEmpty() ? 0 : fm.width(t));
    int x = b.left() + (b.width() - cw) / 2;
    if (align & Qt::AlignLeft)
        x = b.left() + pad;
    else if (align & Qt::AlignRight)
        x = b.right() + 1 - pad - dw - cw;
    if (!icon.isNull()) {
        p.drawPixmap(x, (height() - icon.height()) / 2, dark && !iconInv.isNull() ? iconInv : icon);
        x += icon.width() + g;
    }
    p.setFont(face);
    p.setPen(dark ? Qt::white : Qt::black);
    if (!t.isEmpty())
        p.drawText(QRect(x, 0, qMax(0, b.right() + 1 - pad - dw - x), height()), Qt::AlignLeft | Qt::AlignVCenter, t);
    if (!detail.isEmpty())
        p.drawText(QRect(b.left(), 0, qMax(0, b.width() - pad), height()), Qt::AlignRight | Qt::AlignVCenter, detail);
}

void AioTap::press() {
    down = true;
    if (!feedback)
        return;
    pressTimer->stop();
    if (!pressed) {
        pressed = true;
        update();
    }
}

void AioTap::release(bool inside) {
    const bool was = down;
    down = false;
    if (pressed)
        pressTimer->start(); // the pressed state stays 120 ms, then one repaint back
    if (was && inside)
        emit tapped();
}

void AioTap::unpress() {
    if (pressed && !down) {
        pressed = false;
        update();
    }
}

bool AioTap::event(QEvent *e) {
    switch (e->type()) {
    case QEvent::TouchBegin:
        lastTouch.start();
        press();
        e->accept();
        return true;
    case QEvent::TouchUpdate:
        lastTouch.start();
        e->accept();
        return true;
    case QEvent::TouchEnd: {
        lastTouch.start();
        const QTouchEvent *te = static_cast<QTouchEvent *>(e);
        const bool inside = te->touchPoints().isEmpty() || rect().contains(te->touchPoints().first().pos().toPoint());
        e->accept();
        release(inside);
        return true;
    }
    case QEvent::TouchCancel:
        lastTouch.start();
        e->accept();
        release(false);
        return true;
    case QEvent::MouseButtonDblClick:
    case QEvent::MouseMove:
    case QEvent::Gesture:
    case QEvent::GestureOverride:
        e->accept();
        return true;
    default:
        return QWidget::event(e);
    }
}

void AioTap::mousePressEvent(QMouseEvent *e) {
    e->accept();
    if (recentTouch(lastTouch) || e->button() != Qt::LeftButton)
        return;
    press();
}

void AioTap::mouseReleaseEvent(QMouseEvent *e) {
    e->accept();
    if (recentTouch(lastTouch))
        return;
    release(rect().contains(e->pos()));
}

// -- AioSlider --------------------------------------------------------------------------------------------

AioSlider::AioSlider(QWidget *parent, int tk, int tn, const QPixmap &k)
    : QWidget(parent), value(0), maximum(0), dragValue(0), dragging(false), thick(qMax(1, tk)), thin(qMax(1, tn)),
      drawnX(-1), knob(k) {
    setAttribute(Qt::WA_AcceptTouchEvents);
    setAttribute(Qt::WA_OpaquePaintEvent); // paintEvent fills white itself: nothing below is repainted
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
}

int AioSlider::margin() const {
    return knob.width() / 2 + 1;
}

int AioSlider::xFor(qint64 v) const {
    const int m = margin(), w = width() - 2 * m;
    if (maximum <= 0 || w <= 0)
        return m;
    return m + int(qBound<qint64>(0, v, maximum) * w / maximum);
}

qint64 AioSlider::valueAt(qreal x) const {
    const int m = margin(), w = width() - 2 * m;
    if (maximum <= 0 || w <= 0)
        return 0;
    return qint64(qBound<qreal>(0, (x - m) / w, 1) * maximum);
}

// Dirty band between the knob as drawn and its new place (plus the knob's width), never the whole line.
void AioSlider::moveKnob(int x) {
    if (drawnX < 0) {
        update();
        return;
    }
    if (x == drawnX)
        return;
    const int half = knob.width() / 2 + 2;
    const int kh = qMax(knob.height(), thick) + 4;
    const int left = qMin(x, drawnX) - half, right = qMax(x, drawnX) + half;
    update(QRect(left, height() / 2 - kh / 2, right - left, kh));
}

void AioSlider::setValue(qint64 v, qint64 maxValue) {
    const bool rescale = maxValue != maximum;
    value = v;
    maximum = maxValue;
    if (dragging)
        return;
    if (rescale)
        update(); // new track length: the whole line, once
    else
        moveKnob(xFor(value));
}

void AioSlider::paintEvent(QPaintEvent *e) {
    QPainter p(this);
    p.fillRect(e->rect(), Qt::white);
    const int m = margin(), cy = height() / 2, x = xFor(dragging ? dragValue : value);
    drawnX = x;
    p.fillRect(QRect(m, cy - thin / 2, qMax(0, width() - 2 * m), thin), Qt::black);
    if (x > m)
        p.fillRect(QRect(m, cy - thick / 2, x - m, thick), Qt::black);
    if (maximum > 0 && !knob.isNull())
        p.drawPixmap(x - knob.width() / 2, cy - knob.height() / 2, knob);
}

void AioSlider::press(qreal x) {
    if (maximum <= 0)
        return;
    dragging = true;
    dragValue = valueAt(x);
    moveKnob(xFor(dragValue));
}

void AioSlider::drag(qreal x) {
    if (!dragging)
        return;
    dragValue = valueAt(x);
    const int nx = xFor(dragValue);
    if (qAbs(nx - drawnX) >= qMax(4, width() / 50)) // e-ink: repaint every ~2% of travel
        moveKnob(nx);
}

void AioSlider::release(qreal x) {
    if (!dragging)
        return;
    dragValue = valueAt(x);
    dragging = false;
    value = dragValue;
    moveKnob(xFor(value));
    emit seekRequested(value);
}

bool AioSlider::event(QEvent *e) {
    switch (e->type()) {
    case QEvent::TouchBegin:
    case QEvent::TouchUpdate:
    case QEvent::TouchEnd: {
        lastTouch.start();
        const QTouchEvent *te = static_cast<QTouchEvent *>(e);
        if (!te->touchPoints().isEmpty()) {
            qreal x = te->touchPoints().first().pos().x();
            if (e->type() == QEvent::TouchBegin)
                press(x);
            else if (e->type() == QEvent::TouchUpdate)
                drag(x);
            else
                release(x);
        }
        e->accept();
        return true;
    }
    case QEvent::TouchCancel:
        lastTouch.start();
        if (dragging) {
            dragging = false;
            moveKnob(xFor(value));
        }
        e->accept();
        return true;
    case QEvent::MouseButtonDblClick:
    case QEvent::Gesture:
    case QEvent::GestureOverride:
        e->accept();
        return true;
    default:
        return QWidget::event(e);
    }
}

void AioSlider::mousePressEvent(QMouseEvent *e) {
    if (e->button() == Qt::LeftButton && !recentTouch(lastTouch))
        press(e->pos().x());
    e->accept();
}

void AioSlider::mouseMoveEvent(QMouseEvent *e) {
    if (!recentTouch(lastTouch))
        drag(e->pos().x());
    e->accept();
}

void AioSlider::mouseReleaseEvent(QMouseEvent *e) {
    if (!recentTouch(lastTouch))
        release(e->pos().x());
    e->accept();
}

// -- AioAudio ---------------------------------------------------------------------------------------------

int AioAudio::px(int dragon) const {
    return qMax(1, qRound(dragon * scale));
}

int AioAudio::frameWidth() const {
    return qMax(2, px(2));
}

AioAudio::AioAudio(QWidget *r) : QObject(r), reader(r) {
    resolveMp3z();
    QScreen *screen = QGuiApplication::primaryScreen();
    QSize s = screen ? screen->size() : QSize(1264, 1680);
    const int w = qMax(1, s.width()), h = qMax(1, s.height());
    // Libra Colour / Libra 2: 1264x1680 at 300 ppi ("Dragon"). The rows are a fixed-height column designed for a
    // 1680 px tall portrait screen: the actual height is the budget (in landscape the layout shrinks to fit it;
    // the wider screen has room to spare).
    scale = h / 1680.0;
    if (w > h)
        nh_log("AIO Audio: landscape screen (%dx%d), layout scaled by %.2f", w, h, double(scale));
    // First guess from the screen; relayout() measures the reader on Show and resizes the cover to fit.
    int side = qMax(px(200), qMin(w * 78 / 100, h * 48 / 100));
    coverBox = QSize(side, side);
    const int target = px(106);                  // 9 mm at 300 ppi
    if (QFile::exists(QLatin1String(AIO_DIR "/no-fast-refresh")) && fastFlushOn) {
        fastFlushOn = false;
        nh_log("AIO Audio: fast refresh off (no-fast-refresh)");
    }

    timer = new QTimer(this);
    timer->setInterval(5000);                    // e-ink: one short update every 5 s, only while playing and visible
    connect(timer, SIGNAL(timeout()), this, SLOT(tick()));
    stateTimer = new QTimer(this);
    stateTimer->setSingleShot(true);
    stateTimer->setInterval(400);                // play/pause flips settle for 0.4 s (debounce, 1.5 s at most)
    connect(stateTimer, SIGNAL(timeout()), this, SLOT(applyPlayState()));
    saveTimer = new QTimer(this);
    saveTimer->setSingleShot(true);              // the position save of a pause that came < 15 s after the last
    connect(saveTimer, SIGNAL(timeout()), this, SLOT(savePaused()));
    sleepTimer = new QTimer(this);
    sleepTimer->setSingleShot(true);
    connect(sleepTimer, SIGNAL(timeout()), this, SLOT(sleepFired()));
    volApplyTimer = new QTimer(this);
    volApplyTimer->setSingleShot(true);
    volApplyTimer->setInterval(300);             // one setVolume for a burst of taps
    connect(volApplyTimer, SIGNAL(timeout()), this, SLOT(applyVolumeNow()));
    volSaveTimer = new QTimer(this);
    volSaveTimer->setSingleShot(true);
    volSaveTimer->setInterval(2000);             // one positions.ini write for a burst of taps
    connect(volSaveTimer, SIGNAL(timeout()), this, SLOT(saveVolumeNow()));
    r->installEventFilter(this);                 // Hide/Show: stop and restart the tick timer

    cover = child<QLabel>(r, "coverImage");
    playPause = child<QLabel>(r, "playPause");
    QLabel *back = child<QLabel>(r, "backward"), *fwd = child<QLabel>(r, "forward");
    QLabel *prev = child<QLabel>(r, "prevChapter"), *next = child<QLabel>(r, "nextChapter");
    QLabel *volDown = child<QLabel>(r, "logVolumeDown"), *volUp = child<QLabel>(r, "logVolumeUp");
    QLabel *volume = child<QLabel>(r, "volume"), *title = child<QLabel>(r, "title");
    QBoxLayout *col = child<QBoxLayout>(r, "verticalLayout");
    if (!col || !playPause || !cover) {
        nh_log("AIO Audio: Ui_MP3Reader changed, keeping the stock layout");
        playPause = 0; // stock "Play"/"Pause" text stays
        cover = 0;     // stock cover lambda keeps the cover (never marked aioCover)
        return;
    }

    const QFont fBack = makeFont("DefaultSansSerif", px(30), true);
    const QFont fChip = crisp(makeFont("DefaultSansSerif", px(28), true));  // speed and sleep chips (fast refresh)
    const QFont fTitle = makeFont("DefaultSerif", px(40), true);
    const QFont fChapter = makeFont("DefaultSansSerif", px(30), false, true);
    const QFont fTime = crisp(makeFont("DefaultSansSerif", px(28), false)); // times and volume % (fast refresh)
    const QFont fFoot = makeFont("DefaultSansSerif", px(24), false);
    const bool ownVolume = btVolumeApi();        // headset volume; otherwise the stock log10 buttons stay
    const bool stockVolume = !ownVolume && volDown && volUp && volume;
    const bool seekButtons = back && fwd, chapterButtons = prev && next;

    // The stock widgets we keep leave the form's layouts; everything else in the form is hidden and collapsed.
    QWidget *const keep[] = {cover.data(), playPause.data(), seekButtons ? back : 0, seekButtons ? fwd : 0,
                             chapterButtons ? prev : 0, chapterButtons ? next : 0,
                             stockVolume ? volDown : 0, stockVolume ? volUp : 0, stockVolume ? volume : 0};
    for (QWidget *k : keep)
        detach(r, k);
    collapse(col);
    static const char *const hidden[] = {"title", "current", "total", "volumeUp", "volumeDown", "slower", "faster", "normal", nullptr};
    for (int i = 0; hidden[i]; i++)
        if (QWidget *x = child<QWidget>(r, hidden[i]))
            x->hide();
    if (!stockVolume) {
        QWidget *const unused[] = {volDown, volUp, volume};
        for (QWidget *u : unused)
            if (u)
                u->hide();
    }

    // The 4.45 MP3Reader layout, checked once per reader: without it, no path, no track list, no offset reads.
    layoutOk = readerLayout(r);
    if (!layoutOk) {
        static bool logged = false;
        if (!logged) {
            logged = true;
            nh_log("AIO Audio: MP3Reader layout not recognised (no Volume at +0x24), no track list or book path");
        }
    }

    // The book: its .mp3z path names the title when the tags do not.
    if (layoutOk)
        bookPath = readerPath(r);
    if (!bookPath.isEmpty()) {
        const QFileInfo fi(bookPath);
        const QString dir = fi.dir().dirName();
        const bool generic = dir.isEmpty() || dir.startsWith(QLatin1Char('.')) || dir == QLatin1String("onboard") ||
                             dir == QLatin1String("mnt");
        fallbackTitle = generic ? fi.completeBaseName() : dir;
        fallbackTitle = fallbackTitle.trimmed();
    }

    AioPane *root = new AioPane(r, Qt::white);
    root->setObjectName("aioRoot");
    rootPane = root;
    QVBoxLayout *v = new QVBoxLayout(root);
    const int bottomMargin = px(16);
    v->setContentsMargins(px(32), 0, px(32), bottomMargin);
    v->setSpacing(0);
    const int gapMin = px(8);                    // under every stretch: groups never touch
    int fixed = bottomMargin;
    auto addGap = [v, gapMin, &fixed]() {
        v->addSpacing(gapMin);
        v->addStretch(1);
        fixed += gapMin;
    };

    // Header: [<] Mis libros .............. [toc] (1x)
    const int headerH = px(112);
    AioPane *header = new AioPane(root);
    header->setFixedHeight(headerH);
    QHBoxLayout *hl = new QHBoxLayout(header);
    hl->setContentsMargins(0, 0, 0, 0);
    hl->setSpacing(0);
    if (ReadingController_closeReadingView) {
        const QPixmap arrow = nickelIcon(":/images/reading/back_arrow.png", px(56), G_CHEVRON);
        AioTap *backTap = new AioTap(header, arrow, fBack, false, false); // big target: no pressed repaint
        backTap->setObjectName("aioBack");
        backTap->setAlignment(Qt::AlignLeft);
        backTap->setFixedHeight(headerH);
        backTap->setText(QString::fromUtf8("Mis libros"));
        backTap->fitWidth(QStringList() << backTap->text(), target);
        backTap->setFixedWidth(backTap->width() + px(24));
        connect(backTap, SIGNAL(tapped()), this, SLOT(goHome()));
        hl->addWidget(backTap);
    }
    hl->addStretch(1);
    if (layoutOk && MP3Reader_goToPageFn && !bookPath.isEmpty()) {
        AioTap *tt = new AioTap(header, nickelIcon(":/images/reading/reading_toc.png", px(48), G_TOC), fChip, false, true);
        tt->setObjectName("aioToc");
        tt->setFixedSize(target, headerH);
        connect(tt, SIGNAL(tapped()), this, SLOT(openToc()));
        tocTap = tt;
        hl->addWidget(tt);
        hl->addSpacing(px(12));
        fastWidgets << QPointer<QWidget>(tt);
    }
    AioTap *sp = new AioTap(header, QPixmap(), fChip, true, true);
    sp->setObjectName("aioSpeed");
    sp->setFixedHeight(headerH);
    QStringList rates;
    for (int i = 0; i < RATE_COUNT; i++)
        rates << QString::fromLatin1(RATE_NAMES[i]);
    sp->fitWidth(rates, target);
    sp->setText(rates.at(speedIndex));
    connect(sp, SIGNAL(tapped()), this, SLOT(cycleSpeed()));
    speedTap = sp;
    fastWidgets << QPointer<QWidget>(sp);
    hl->addWidget(sp);
    v->addWidget(header);
    AioPane *line = new AioPane(root, Qt::black);
    const int lineH = qMax(2, px(2));
    line->setFixedHeight(lineH);
    v->addWidget(line);
    fixed += headerH + lineH;
    addGap();

    // Cover, centred, framed (its size follows the reader's height: relayout()).
    AioPane *cvr = new AioPane(root);
    cvr->setFixedHeight(side);
    coverRow = cvr;
    QHBoxLayout *cr = new QHBoxLayout(cvr);
    cr->setContentsMargins(0, 0, 0, 0);
    cr->setSpacing(0);
    cover->setFixedSize(coverBox);
    cover->setAlignment(Qt::AlignCenter);
    cover->setProperty("aioCoverSlot", true); // the stock cover lambda may not move it, its pictures get framed
    cr->addStretch(1);
    cr->addWidget(cover);
    cr->addStretch(1);
    v->addWidget(cvr);
    if (const QPixmap *pm = cover->pixmap())
        if (!pm->isNull())
            cover->setPixmap(framedCover(*pm));
    addGap();

    // Book title and chapter, one line each.
    const int titleH = qMax(px(56), QFontMetrics(fTitle).height() + px(6));
    AioText *tt = new AioText(root, fTitle, Qt::AlignHCenter);
    tt->setFixedHeight(titleH);
    titleText = tt;
    if (!fallbackTitle.isEmpty())
        tt->setText(fallbackTitle); // until a TALB arrives
    v->addWidget(tt);
    const int chapterH = qMax(px(44), QFontMetrics(fChapter).height() + px(6));
    AioText *ct = new AioText(root, fChapter, Qt::AlignHCenter);
    ct->setFixedHeight(chapterH);
    chapterText = ct;
    v->addWidget(ct);
    fixed += titleH + chapterH;
    addGap();

    // Scrubber, then elapsed / -remaining 8 px under it, aligned with the ends of the line (the 5 s tick touches
    // only this band).
    const QPixmap knob = nickelIcon(":/images/reading/scrubber_handle_off.png", px(56), G_KNOB);
    AioSlider *sl = new AioSlider(root, qMax(4, px(6)), qMax(2, px(2)), knob);
    const int sliderH = knob.height() + px(16);
    sl->setFixedHeight(sliderH);
    connect(sl, SIGNAL(seekRequested(qint64)), this, SLOT(seekTo(qint64)));
    slider = sl;
    fastWidgets << QPointer<QWidget>(sl);
    v->addWidget(sl);
    v->addSpacing(px(8));
    const int timeH = qMax(px(40), QFontMetrics(fTime).height() + px(4));
    AioPane *times = new AioPane(root, Qt::white);
    times->setFixedHeight(timeH);
    QHBoxLayout *tl = new QHBoxLayout(times);
    const int knobHalf = knob.width() / 2 + 1; // AioSlider::margin(): where the line starts and ends
    tl->setContentsMargins(knobHalf, 0, knobHalf, 0);
    tl->setSpacing(0);
    AioText *el = new AioText(times, fTime, Qt::AlignLeft);
    el->setFixedSize(clockWidth(fTime, false) + px(8), timeH);
    AioText *rem = new AioText(times, fTime, Qt::AlignRight);
    rem->setFixedSize(clockWidth(fTime, true) + px(8), timeH);
    tl->addWidget(el);
    tl->addStretch(1);
    tl->addWidget(rem);
    elapsedText = el;
    remainText = rem;
    el->setText(QString::fromLatin1("0:00"));
    fastWidgets << QPointer<QWidget>(times) << QPointer<QWidget>(el) << QPointer<QWidget>(rem);
    v->addWidget(times);
    fixed += sliderH + px(8) + timeH;
    addGap();

    // Transport: [|<] [-30] [play/pause] [+30] [>|], evenly spaced.
    playIcon = nickelIcon(":/images/audiobook/play_button.png", px(158), G_PLAY);
    pauseIcon = nickelIcon(":/images/audiobook/pause_button.png", px(158), G_PAUSE);
    const int pb = qMax(target, px(158) + px(12));
    playBox = QSize(pb, pb);
    AioPane *transport = new AioPane(root);
    transport->setFixedHeight(pb);
    QHBoxLayout *xl = new QHBoxLayout(transport);
    xl->setContentsMargins(0, 0, 0, 0);
    xl->setSpacing(0);
    xl->addStretch(1);
    auto addButton = [xl, this](QWidget *b) {
        xl->addWidget(b, 0, Qt::AlignVCenter);
        xl->addStretch(1);
        fastWidgets << QPointer<QWidget>(b); // icons only: black and white
    };
    const QSize cb(target, target);
    const int sb = qMax(target, px(100) + px(12));
    if (chapterButtons) { // stock goToPrevPage/goToNextPage stay wired
        prev->setFixedSize(cb);
        prev->setAlignment(Qt::AlignCenter);
        prev->setText(QString());
        setIcon(prev, glyph(G_SKIP_PREV, px(60)), cb);
        prev->setProperty("aioOwned", true);
        onTap(prev, this, "userSkip()");
        addButton(prev);
    }
    if (seekButtons) {
        back->setFixedSize(sb, sb);
        back->setAlignment(Qt::AlignCenter);
        setIcon(back, nickelIcon(":/images/audiobook/backward_button.png", px(100), G_BACK30), QSize(sb, sb));
        back->setProperty("aioOwned", true);
        onTap(back, this, "rewind()"); // stock: empty slots
        addButton(back);
    }
    playPause->setFixedSize(playBox);
    playPause->setAlignment(Qt::AlignCenter);
    playPause->setProperty("aioOwned", true);
    if (TouchLabel_setSelectedPixmap && playPause->inherits("TouchLabel")) { // pressed states, once per reader
        playIconInv = inverted(playIcon, playBox);
        pauseIconInv = inverted(pauseIcon, playBox);
    }
    playing = playPause->text() == QString::fromUtf8("Pausa") || playPause->text() == QLatin1String("Pause");
    setIcon(playPause, playing ? pauseIcon : playIcon, playBox, playing ? pauseIconInv : playIconInv);
    addButton(playPause);
    if (seekButtons) {
        fwd->setFixedSize(sb, sb);
        fwd->setAlignment(Qt::AlignCenter);
        setIcon(fwd, nickelIcon(":/images/audiobook/forward_button.png", px(100), G_FWD30), QSize(sb, sb));
        fwd->setProperty("aioOwned", true);
        onTap(fwd, this, "ahead()");
        addButton(fwd);
    }
    if (chapterButtons) {
        next->setFixedSize(cb);
        next->setAlignment(Qt::AlignCenter);
        next->setText(QString());
        setIcon(next, glyph(G_SKIP_NEXT, px(60)), cb);
        next->setProperty("aioOwned", true);
        onTap(next, this, "userSkip()");
        addButton(next);
    }
    v->addWidget(transport);
    fixed += pb;
    addGap();

    // Bottom bar: [-] 55% [+] ....... (timer 15 min) ....... [bt] name. Equal side groups keep the chip centred.
    AioPane *bar = new AioPane(root);
    bar->setFixedHeight(target);
    QHBoxLayout *bl = new QHBoxLayout(bar);
    bl->setContentsMargins(0, 0, 0, 0);
    bl->setSpacing(0);
    const int sideW = qMax(px(320), 2 * target + textWidth(fTime, QString::fromLatin1("100%")) + px(16));
    AioPane *volGroup = new AioPane(bar);
    volGroup->setFixedSize(sideW, target);
    QHBoxLayout *vg = new QHBoxLayout(volGroup);
    vg->setContentsMargins(0, 0, 0, 0);
    vg->setSpacing(0);
    const int volW = textWidth(fTime, QString::fromLatin1("100%")) + px(16);
    if (ownVolume) {
        AioTap *dn = new AioTap(volGroup, glyph(G_MINUS, px(48)), fTime, false, true);
        dn->setObjectName("aioVolumeDown");
        dn->setFixedSize(cb);
        AioText *vt = new AioText(volGroup, fTime, Qt::AlignHCenter);
        vt->setFixedSize(volW, target);
        AioTap *up = new AioTap(volGroup, glyph(G_PLUS, px(48)), fTime, false, true);
        up->setObjectName("aioVolumeUp");
        up->setFixedSize(cb);
        connect(dn, SIGNAL(tapped()), this, SLOT(volumeDown()));
        connect(up, SIGNAL(tapped()), this, SLOT(volumeUp()));
        vg->addWidget(dn);
        vg->addWidget(vt);
        vg->addWidget(up);
        volText = vt;
        fastWidgets << QPointer<QWidget>(dn) << QPointer<QWidget>(vt) << QPointer<QWidget>(up);
        showVolume();
    } else if (stockVolume) { // stock log10 stream volume, restyled
        QLabel *const vols[] = {volDown, volUp};
        for (QLabel *l : vols) {
            l->setFixedSize(cb);
            l->setAlignment(Qt::AlignCenter);
            l->setProperty("aioOwned", true);
        }
        setIcon(volDown, glyph(G_MINUS, px(48)), cb);
        setIcon(volUp, glyph(G_PLUS, px(48)), cb);
        if (volume->text().startsWith(QString::fromUtf8("Volumen ")))
            volume->setText(volume->text().mid(8));
        volume->setAlignment(Qt::AlignCenter);
        volume->setFixedSize(volW, target);
        QFont vf = volume->font(); // the stylesheet sets family and size only: the strategy stays
        vf.setStyleStrategy(QFont::NoAntialias);
        volume->setFont(vf);
        vg->addWidget(volDown);
        vg->addWidget(volume);
        vg->addWidget(volUp);
        fastWidgets << QPointer<QWidget>(volDown) << QPointer<QWidget>(volume) << QPointer<QWidget>(volUp);
    }
    vg->addStretch(1);
    bl->addWidget(volGroup);
    bl->addStretch(1);
    const QPixmap timerIcon = nickelIcon(":/images/reading/timer.png", px(48), G_TIMER);
    AioTap *st = new AioTap(bar, timerIcon, fChip, true, true);
    st->setObjectName("aioSleep");
    st->setFixedHeight(target);
    st->fitWidth(QStringList() << QString::fromUtf8("Apagado") << QString::fromUtf8("Fin de pista")
                               << QString::fromUtf8("60 min"), target);
    connect(st, SIGNAL(tapped()), this, SLOT(cycleSleep()));
    sleepTap = st;
    fastWidgets << QPointer<QWidget>(st);
    bl->addWidget(st);
    bl->addStretch(1);
    AioTap *bt = new AioTap(bar, nickelIcon(":/images/reading/bluetooth.png", px(54), G_BT), fTime, false, false);
    bt->setObjectName("aioBluetooth");
    bt->setAlignment(Qt::AlignRight);
    bt->setFixedSize(sideW, target);
    connect(bt, SIGNAL(tapped()), this, SLOT(btTapped())); // consumes the tap, nothing heavy
    btTap = bt;
    bl->addWidget(bt);
    v->addWidget(bar);
    fixed += target;
    v->addSpacing(gapMin);
    fixed += gapMin;

    // Footer: track and whole-book progress, text only (its height from the font: never clipped).
    const int footH = QFontMetrics(fFoot).height() + px(8);
    AioText *ft = new AioText(root, fFoot, Qt::AlignHCenter);
    ft->setFixedHeight(footH);
    footerText = ft;
    v->addWidget(ft);
    fixed += footH;
    fixedH = fixed;

    col->insertWidget(0, root, 1);
    col->invalidate();
    if (title)
        setChapter(title->property("aioName").toString());
    updateSleepLabel();
    updateBtChip();

    // Stock widgets only (ours paint themselves), set once: black on white, no borders, no grey fills.
    static const char *const ids[] = {"coverImage", "playPause", "backward", "forward", "prevChapter", "nextChapter",
                                      "logVolumeDown", "logVolumeUp", "volume", nullptr};
    QStringList plain;
    for (int i = 0; ids[i]; i++)
        plain << QString("#%1").arg(QString::fromLatin1(ids[i]));
    r->setStyleSheet(plain.join(", ") + " { border: none; background: white; color: black; margin: 0; padding: 0; }" +
                     QString("#volume { font-family: DefaultSansSerif; font-size: %1px; }").arg(px(28)));

    installGestureFilters(); // again on every Show: Nickel may reparent the reader after its constructor
    refreshWindow();
    relayout();
    schedule();
}

// The reader's top-level QWindow (none until it is shown): only flushes of that window may use the fast refresh.
void AioAudio::refreshWindow() {
    QWidget *top = reader ? reader->window() : 0;
    readerWin = top ? top->windowHandle() : 0;
}

AioAudio::~AioAudio() {
    if (btMine)
        btQueued = false; // our deferred read dies with us
    setKeyFilter(false);
    for (int i = 0; i < gestureHosts.size(); i++)
        if (QWidget *w = gestureHosts.at(i).data())
            w->removeEventFilter(this);
    gestureHosts.clear();
    if (!inReaderCtor)
        forgetMedia(); // a pending track can only be replayed by a reader under construction
    if (volSaveTimer && volSaveTimer->isActive())
        saveVolumeNow();
    saveState(false); // the player may already be stopped or gone: use the last position seen
}

// Swallowing QEvent::Gesture in our pane does not reach Nickel's recognizers: their gestures are grabbed by the
// reading view's ancestors and delivered there. Filter every ancestor of the reader (the reader itself is
// already filtered for Hide/Show).
void AioAudio::installGestureFilters() {
    if (!reader || !rootPane)
        return; // stock layout: Nickel's gestures stay as they are
    for (QWidget *w = reader->parentWidget(); w; w = w->parentWidget()) {
        bool known = false;
        for (int i = 0; i < gestureHosts.size() && !known; i++)
            known = gestureHosts.at(i).data() == w;
        if (known)
            continue;
        w->installEventFilter(this);
        gestureHosts << QPointer<QWidget>(w);
    }
}

// Every gesture of the event has its hot spot (global coordinates) on our pane, while the reader is shown.
bool AioAudio::gestureOnPane(QGestureEvent *ge) const {
    if (!reader || !reader->isVisible() || !rootPane || !rootPane->isVisible())
        return false;
    const QList<QGesture *> gs = ge->gestures();
    if (gs.isEmpty())
        return false;
    const QRect area(rootPane->mapToGlobal(QPoint(0, 0)), rootPane->size());
    for (int i = 0; i < gs.size(); i++) {
        const QGesture *g = gs.at(i);
        if (!g || !g->hasHotSpot() || !area.contains(g->hotSpot().toPoint()))
            return false;
    }
    return true;
}

bool AioAudio::isGestureHost(QObject *o) const {
    for (int i = 0; i < gestureHosts.size(); i++)
        if (gestureHosts.at(i).data() == o)
            return true;
    return false;
}

bool AioAudio::eventFilter(QObject *watched, QEvent *e) {
    const QEvent::Type type = e->type();
    if (type == QEvent::Gesture || type == QEvent::GestureOverride) {
        QGestureEvent *ge = static_cast<QGestureEvent *>(e);
        // only at the reader's ancestors: gestures of the stock buttons inside our pane go on to them
        if ((watched == reader.data() || isGestureHost(watched)) && gestureOnPane(ge)) { // ours: accepted, so Qt does not offer it further
            const QList<QGesture *> gs = ge->gestures();
            for (int i = 0; i < gs.size(); i++)
                ge->accept(gs.at(i));
            e->accept();
            return true;
        }
    } else if (watched == reader.data()) {
        if (type == QEvent::Hide) {
            timer->stop(); // nothing repaints behind other screens
            setKeyFilter(false);
        } else if (type == QEvent::Show) {
            refreshWindow();
            installGestureFilters();
            setKeyFilter(true);
            relayout();
            tick();
            if (playing && !timer->isActive())
                timer->start();
        } else if (type == QEvent::Resize) {
            relayout();
        }
    }
    return QObject::eventFilter(watched, e);
}

bool AioAudio::readerShown() const {
    return reader && reader->isVisible();
}

// The page-turn buttons, only while our player is shown: the app filter goes on and off with the reader.
void AioAudio::setKeyFilter(bool on) {
    QCoreApplication *app = QCoreApplication::instance();
    if (!app)
        return;
    if (on) {
        if (!rootPane)
            return; // stock layout: the page keys stay Nickel's
        if (!keyFilter)
            keyFilter = new AioKeys(app);
        app->installEventFilter(keyFilter); // Qt drops a previous install of the same filter first
        keysOn = true;
    } else if (keysOn) {
        keysOn = false;
        if (keyFilter && (aio.isNull() || aio.data() == this)) // a newer reader keeps its filter
            app->removeEventFilter(keyFilter);
    }
}

bool AioKeys::eventFilter(QObject *watched, QEvent *e) {
    const QEvent::Type t = e->type();
    if (t != QEvent::KeyPress && t != QEvent::KeyRelease)
        return false; // every event of the application passes here: nothing else is looked at
    return aio && aio->pageKey(watched, static_cast<QKeyEvent *>(e));
}

// Nickel's page keys (4.45, RE): TactileButtonFilter turns the page buttons (F23/F24) into Key_Up/Key_Down or
// Key_Left/Key_Right (screen orientation and the "invert page turn buttons" setting applied) and sends them to the
// current view; ReadingView::keyPressEvent pages forward on Key_Right/Key_Down and back on Key_Left/Key_Up
// (shouldPageForward, left-to-right). Here: -30/+30 s, or the track list's pages. Media keys are not touched.
bool AioAudio::pageKey(QObject *receiver, QKeyEvent *ke) {
    int dir = 0;
    switch (ke->key()) {
    case Qt::Key_Right:
    case Qt::Key_Down:
        dir = 1;
        break;
    case Qt::Key_Left:
    case Qt::Key_Up:
        dir = -1;
        break;
    default:
        return false;
    }
    if (!rootPane || !readerShown() || !receiver || !receiver->isWidgetType())
        return false;
    QWidget *w = static_cast<QWidget *>(receiver);
    QWidget *r = reader.data();
    if (w != r && !r->isAncestorOf(w) && !w->isAncestorOf(r) && !w->inherits("ReadingView"))
        return false; // another screen or a dialog over the player
    if (QApplication::activePopupWidget() || QApplication::activeModalWidget())
        return false;
    if (ke->type() == QEvent::KeyRelease)
        return true; // the release of a key we took
    if (ke->isAutoRepeat() && keyClock.isValid() && keyClock.elapsed() < 400)
        return true; // a held button: one step per 400 ms
    keyClock.start();
    if (tocPane && tocPane->isVisible()) {
        if (dir > 0)
            tocNext();
        else
            tocPrev();
    } else if (dir > 0) {
        ahead();
    } else {
        rewind();
    }
    return true;
}

// An enum property of the player (QMediaPlayer::State, MediaStatus) as an int, or -1.
static int enumProperty(QObject *p, const char *name) {
    if (!p)
        return -1;
    const QMetaObject *mo = p->metaObject();
    const int i = mo->indexOfProperty(name);
    if (i < 0)
        return -1;
    const QMetaProperty prop = mo->property(i);
    if (!prop.isEnumType())
        return -1;
    const QVariant v = prop.read(p);
    if (!v.isValid() || !v.constData())
        return -1;
    return *static_cast<const int *>(v.constData());
}

// Track changes are let through when the track is over: whatever libmp3z/Nickel uses to advance keeps working.
// Besides the player's own position and EndOfMedia: the last position seen (ticks, positionChanged) was within 3 s
// of the track's end, or the player went from Playing to Stopped near it (a backend that already rewound to 0).
// The stock prev/next buttons and media keys never get here (blockNav only looks at qt_static_metacall).
bool AioAudio::trackEnded() const {
    QObject *p = player.data();
    if (!p)
        return true;
    const qint64 pos = p->property("position").toLongLong(), dur = p->property("duration").toLongLong();
    if (dur > 0 && pos >= dur - 1500)
        return true;
    if (enumProperty(p, "mediaStatus") == 7) // EndOfMedia
        return true;
    if (lastDur > 0 && lastPos >= lastDur - 3000)
        return true;
    const int state = enumProperty(p, "state");
    if (state == 0 && prevPlayerState == 1 && lastDur > 0 && lastPos >= lastDur - 10000) // Stopped after Playing
        return true;
    return false;
}

bool AioAudio::inFast(const QRect &g) const {
    for (int i = 0; i < fastWidgets.size(); i++) {
        const QWidget *w = fastWidgets.at(i).data();
        if (w && w->isVisible() && QRect(w->mapToGlobal(QPoint(0, 0)), w->size()).contains(g))
            return true;
    }
    return false;
}

// The flush is for the reader's own window, every rect of the flushed region (window coordinates) is inside one of
// our black and white controls, the player is shown and the track list is closed. The first flush of that window
// after a track change keeps the default waveform (one clean refresh of the fast widgets).
bool AioAudio::fastRegion(const QRegion &region, QWindow *win) {
    if (!win || win != readerWin.data())
        return false;
    if (normalFlush) {
        normalFlush = false;
        return false;
    }
    if (!rootPane || !readerShown() || (tocPane && tocPane->isVisible()))
        return false;
    const QPoint origin = win->mapToGlobal(QPoint(0, 0));
    const QRect b = region.boundingRect();
    if (b.isEmpty())
        return false;
    if (inFast(b.translated(origin)))
        return true;
    if (region.rectCount() <= 1)
        return false;
    const QVector<QRect> rs = region.rects();
    for (int i = 0; i < rs.size(); i++)
        if (!inFast(rs.at(i).translated(origin)))
            return false;
    return true;
}

// The cover: the largest square that fits the reader's height once every fixed row is placed.
void AioAudio::relayout() {
    if (!rootPane || !reader || !cover || !coverRow)
        return;
    const int h = reader->height(), w = reader->width();
    if (h < px(400) || w < px(300))
        return; // not laid out yet
    int side = qMin((w - 2 * px(32)) * 78 / 100, h * 48 / 100);
    side = qMin(side, h - fixedH);
    side = qMax(side, px(160));
    if (side == coverBox.width() && coverRow->minimumHeight() == side)
        return;
    coverBox = QSize(side, side);
    cover->setFixedSize(coverBox);
    coverRow->setFixedHeight(side);
    redrawCover();
    if (tocPane && tocPane->isVisible()) {
        tocPane->setGeometry(rootPane->rect());
        layoutToc();
        fillToc(); // rows per page may have changed
    }
}

void AioAudio::redrawCover() {
    if (!cover)
        return;
    if (!coverSrc.isNull())
        cover->setPixmap(framed(coverSrc, coverBox, frameWidth())); // straight to QtWidgets (not hooked)
    else if (placeholder)
        drawPlaceholder();
}

// No picture anywhere yet: a framed box with the book title (the stock lambda may still replace it).
void AioAudio::drawPlaceholder() {
    if (!cover)
        return;
    const int lw = frameWidth();
    QPixmap ph(coverBox);
    ph.fill(Qt::white);
    QPainter p(&ph);
    drawFrame(p, QRect(QPoint(0, 0), coverBox), lw);
    p.setRenderHint(QPainter::TextAntialiasing);
    p.setFont(makeFont("DefaultSerif", px(44), true));
    p.setPen(Qt::black);
    p.drawText(QRect(QPoint(0, 0), coverBox).adjusted(px(48), px(48), -px(48), -px(48)),
               Qt::AlignCenter | Qt::TextWordWrap, bookTitle.isEmpty() ? QString::fromUtf8("Audiolibro") : bookTitle);
    p.end();
    placeholder = true;
    cover->setPixmap(ph);
}

// -- track list ("Capitulos") -------------------------------------------------------------------------------

// libmp3z's current track (MP3Reader+0x30, 0-based, 4.45 RE, validated layout only), checked against the list;
// else ours.
int AioAudio::currentTrack() const {
    int k = -1;
    if (reader && layoutOk)
        k = *reinterpret_cast<const int *>(reinterpret_cast<const char *>(reader.data()) + 0x30);
    if (k < 0 || k >= tocNames.size())
        k = track >= 0 && track < tocNames.size() ? track : -1;
    return k;
}

// libmp3z's own track count: the size of its QVector<Shortcover> (MP3Reader+0x2c, 4.45 RE; Qt 5 QArrayData:
// ref, size, ...). 0 when the layout is not validated or the value is not plausible.
int AioAudio::stockTrackCount() const {
    if (!reader || !layoutOk)
        return 0;
    const int *d = *reinterpret_cast<const int *const *>(reinterpret_cast<const char *>(reader.data()) + 0x2c);
    if (!d)
        return 0;
    const int n = d[1];
    return n > 0 && n <= MAX_TRACKS ? n : 0;
}

void AioAudio::buildToc() {
    AioPane *pane = new AioPane(rootPane, Qt::white);
    pane->setObjectName("aioTocPane");
    tocPane = pane;
    const QFont fTitle = makeFont("DefaultSerif", px(40), true);
    const QFont fChip = makeFont("DefaultSansSerif", px(28), true);
    const QFont fRow = makeFont("DefaultSansSerif", px(32), false);
    const QFont fPage = makeFont("DefaultSansSerif", px(26), false);
    AioText *t = new AioText(pane, fTitle, Qt::AlignLeft);
    t->setText(QString::fromUtf8("Capítulos"));
    tocTitle = t;
    AioTap *close = new AioTap(pane, QPixmap(), fChip, true, true);
    close->setText(QString::fromUtf8("Cerrar"));
    connect(close, SIGNAL(tapped()), this, SLOT(closeToc()));
    tocClose = close;
    tocLine = new AioPane(pane, Qt::black);
    for (int i = 0; i < 12; i++) {
        AioTap *row = new AioTap(pane, QPixmap(), fRow, false, true);
        row->setAlignment(Qt::AlignLeft);
        row->setInset(px(16));
        row->setProperty("aioIndex", -1);
        connect(row, SIGNAL(tapped()), this, SLOT(tocRowTapped()));
        tocRows << QPointer<AioTap>(row);
    }
    AioTap *pv = new AioTap(pane, QPixmap(), fChip, true, true);
    pv->setText(QString::fromUtf8("‹ Anterior"));
    connect(pv, SIGNAL(tapped()), this, SLOT(tocPrev()));
    tocPrevTap = pv;
    AioTap *nx = new AioTap(pane, QPixmap(), fChip, true, true);
    nx->setText(QString::fromUtf8("Siguiente ›"));
    connect(nx, SIGNAL(tapped()), this, SLOT(tocNext()));
    tocNextTap = nx;
    tocPageText = new AioText(pane, fPage, Qt::AlignHCenter);
    AioText *note = new AioText(pane, fPage, Qt::AlignLeft);
    note->setText(QString::fromUtf8("No se pudo verificar el orden de pistas"));
    note->hide();
    tocNote = note;
}

// Manual geometry over the whole reader: header, about 9 rows of at least 106 px, page buttons at the bottom.
void AioAudio::layoutToc() {
    if (!tocPane || !tocClose || !tocTitle || !tocLine || !tocPrevTap || !tocNextTap || !tocPageText)
        return;
    const int W = tocPane->width(), H = tocPane->height();
    const int m = px(32), target = px(106), headerH = px(112), lineH = qMax(2, px(2));
    tocClose->setFixedHeight(headerH);
    tocClose->fitWidth(QStringList() << tocClose->text(), target);
    const int closeW = tocClose->width();
    tocClose->setGeometry(W - m - closeW, 0, closeW, headerH);
    tocTitle->setGeometry(m, 0, qMax(0, W - 3 * m - closeW), headerH);
    tocLine->setGeometry(m, headerH, qMax(0, W - 2 * m), lineH);
    const int navH = target, navY = H - navH - px(16);
    tocPrevTap->setFixedHeight(navH);
    tocNextTap->setFixedHeight(navH);
    tocPrevTap->fitWidth(QStringList() << tocPrevTap->text(), target);
    tocNextTap->fitWidth(QStringList() << tocNextTap->text(), target);
    const int pw = tocPrevTap->width(), nw = tocNextTap->width();
    tocPrevTap->setGeometry(m, navY, pw, navH);
    tocNextTap->setGeometry(W - m - nw, navY, nw, navH);
    tocPageText->setGeometry(m + pw, navY, qMax(0, W - 2 * m - pw - nw), navH);
    int top = headerH + lineH + px(8);
    if (tocNote) { // read-only list: a short note under the line, rows below it
        const int noteH = QFontMetrics(makeFont("DefaultSansSerif", px(26), false)).height() + px(8);
        tocNote->setGeometry(m, top, qMax(0, W - 2 * m), noteH);
        const bool showNote = !tocVerified && !tocNames.isEmpty();
        tocNote->setVisible(showNote);
        if (showNote)
            top += noteH + px(4);
    }
    const int avail = qMax(0, navY - px(8) - top);
    int rows = 9, rowH = avail / rows;
    if (rowH < target) {
        rows = qMax(1, avail / target);
        rowH = qMax(1, avail / rows);
    }
    rows = qMin(rows, tocRows.size());
    tocPerPage = rows;
    for (int i = 0; i < tocRows.size(); i++) {
        AioTap *row = tocRows.at(i).data();
        if (!row)
            continue;
        if (i < rows) {
            row->setFixedHeight(rowH);
            row->setGeometry(m, top + i * rowH, qMax(0, W - 2 * m), rowH);
            row->show();
        } else {
            row->hide();
        }
    }
}

void AioAudio::fillToc() {
    const int n = tocNames.size(), per = qMax(1, tocPerPage);
    const int pages = qMax(1, (n + per - 1) / per);
    tocPage = qBound(0, tocPage, pages - 1);
    for (int i = 0; i < per && i < tocRows.size(); i++) {
        AioTap *row = tocRows.at(i).data();
        if (!row)
            continue;
        const int k = tocPage * per + i;
        if (k < n) {
            QString name = tocNames.at(k);
            name = name.mid(name.lastIndexOf('/') + 1); // folder entries keep their own name
            QString clean = dedupe(cleanTrackName(name));
            if (clean.isEmpty())
                clean = name;
            row->setText(clean);
            row->setDetail(QString::number(k + 1));
            row->setInverted(k == tocCurrent);
            row->setProperty("aioIndex", k);
        } else {
            row->setText(n == 0 && i == 0 ? QString::fromUtf8("No se pudo leer la lista de pistas") : QString());
            row->setDetail(QString());
            row->setInverted(false);
            row->setProperty("aioIndex", -1);
        }
    }
    if (tocPageText)
        tocPageText->setText(QString::fromUtf8("Página %1 de %2").arg(tocPage + 1).arg(pages));
}

void AioAudio::openToc() {
    if (!rootPane || !reader)
        return;
    if (!tocLoaded) { // once per reader, on the first open
        tocLoaded = true;
        tocNames = zipNames(bookPath);
        if (tocNames.isEmpty())
            nh_log("AIO Audio: could not read the track list of the .mp3z");
    }
    // Jumps only when the list matches the player's own count (libmp3z's vector, else TRCK), when one is known.
    const int stock = stockTrackCount(), known = stock > 0 ? stock : trackCount;
    tocVerified = !tocNames.isEmpty() && (known <= 0 || tocNames.size() == known);
    if (!tocNames.isEmpty() && !tocVerified) {
        static bool logged = false;
        if (!logged) {
            logged = true;
            nh_log("AIO Audio: %d entries in the .mp3z, the player has %d (%s): track list read-only", tocNames.size(),
                   known, stock > 0 ? "libmp3z" : "TRCK");
        }
    }
    if (!tocPane)
        buildToc();
    tocPane->setGeometry(rootPane->rect());
    layoutToc();
    tocCurrent = tocVerified ? currentTrack() : -1;
    tocPage = tocCurrent >= 0 ? tocCurrent / qMax(1, tocPerPage) : 0;
    fillToc();
    tocPane->raise();
    tocPane->show();
}

void AioAudio::closeToc() {
    if (tocPane && tocPane->isVisible()) {
        tocPane->hide();
        QTimer::singleShot(0, this, SLOT(tick())); // what changed underneath while the list was open
    }
}

void AioAudio::tocPrev() {
    if (!tocPane || !tocPane->isVisible() || tocPage <= 0)
        return;
    tocPage--;
    fillToc();
}

void AioAudio::tocNext() {
    if (!tocPane || !tocPane->isVisible() || (tocPage + 1) * qMax(1, tocPerPage) >= tocNames.size())
        return;
    tocPage++;
    fillToc();
}

void AioAudio::tocRowTapped() {
    AioTap *row = qobject_cast<AioTap *>(sender());
    if (!row)
        return;
    const int k = row->property("aioIndex").toInt();
    if (!tocVerified || k < 0 || k >= tocNames.size())
        return; // read-only list: the order could not be checked against the player
    closeToc();
    if (k == currentTrack())
        return; // already playing: nothing to reload, the position stays
    jumpTarget = k;
    QTimer::singleShot(0, this, SLOT(jumpNow())); // not inside the tap
}

// goToPage (1-based) -> setPage(k) -> setMedia: setTrack sees the change and saves the new track from 0:00.
void AioAudio::jumpNow() {
    int k = jumpTarget;
    jumpTarget = -1;
    QWidget *r = reader.data();
    const int count = tocNames.size();
    if (k < 0 || !r || !MP3Reader_goToPageFn || !tocVerified || count <= 0)
        return;
    k = qBound(0, k, count - 1);
    userSkip();       // not an "end of track" for the sleep timer
    pendingSeek = -1; // no resume over the chosen track
    resumeTarget = -1;
    QPointer<AioAudio> self(this);
    MP3Reader_goToPageFn(r, k + 1); // the function itself, not the PLT entry we filter
    if (!self || !reader)
        return;
    if (MP3Reader_saveBookmark)
        MP3Reader_saveBookmark(reader.data());
}

// The tick timer runs only while playing and visible.
void AioAudio::schedule() {
    if (playing && reader && reader->isVisible()) {
        if (!timer->isActive())
            timer->start();
    } else {
        timer->stop();
    }
}

// `inv`: the pressed state already built (play/pause flips often), otherwise built here.
void AioAudio::setIcon(QLabel *l, const QPixmap &pm, const QSize &box, const QPixmap &inv) {
    if (!l || pm.isNull())
        return;
    l->setPixmap(pm); // straight to QtWidgets: our setPixmap hook only covers libmp3z's PLT
    if (l->inherits("TouchLabel")) { // the TouchLabel paints its own pressed state with the selected pixmap
        if (TouchLabel_setDeselectedPixmap)
            TouchLabel_setDeselectedPixmap(l, pm);
        if (TouchLabel_setSelectedPixmap)
            TouchLabel_setSelectedPixmap(l, inv.isNull() ? inverted(pm, box) : inv);
    }
}

QPixmap AioAudio::framedCover(const QPixmap &pm) {
    if (pm.isNull())
        return pm;
    coverSrc = pm.toImage(); // re-framed if relayout() changes the box
    placeholder = false;
    return framed(coverSrc, coverBox, frameWidth());
}

void AioAudio::setPlayer(QObject *p) {
    if (!p || !p->inherits("QMediaPlayer") || player.data() == p)
        return;
    player = p;
    connect(p, SIGNAL(durationChanged(qint64)), this, SLOT(onDuration()));
    // Only two ints are stored per signal (no repaint): the last position and the state before the current one.
    connect(p, SIGNAL(positionChanged(qint64)), this, SLOT(onPosition(qint64)));
    connect(p, SIGNAL(stateChanged(QMediaPlayer::State)), this, SLOT(onState()));
    playerState = enumProperty(p, "state");
    prevPlayerState = -1;
    qreal rate = p->property("playbackRate").toReal();
    int best = 0;
    for (int i = 1; i < RATE_COUNT; i++)
        if (qAbs(RATES[i] - rate) < qAbs(RATES[best] - rate))
            best = i;
    speedIndex = rate > 0 ? best : 0;
    if (speedTap)
        speedTap->setText(QString::fromLatin1(RATE_NAMES[speedIndex]));
}

// Validated 4.45 layout: the reader's own AudioPlayer (MP3Reader+0x40). Otherwise no offset is read: any
// QMediaPlayer set while this is the current reader is taken as its own (one MP3Reader at a time).
bool AioAudio::owns(QObject *p) const {
    if (!p || !reader)
        return false;
    if (layoutOk)
        return readerPlayer(reader.data()) == p;
    return p->inherits("QMediaPlayer");
}

// The last position seen. A stopped player back at 0 (end of media on some backends) keeps the last one, so
// trackEnded() still knows the track ran out.
void AioAudio::notePos(qint64 pos) {
    if (pos <= 0 && lastPos > 0 && (playerState == 0 || enumProperty(player.data(), "mediaStatus") == 7))
        return;
    lastPos = qMax<qint64>(0, pos);
}

void AioAudio::onPosition(qint64 pos) {
    notePos(pos);
}

void AioAudio::onState() {
    const int s = enumProperty(player.data(), "state");
    if (s == playerState)
        return;
    prevPlayerState = playerState;
    playerState = s;
}

void AioAudio::media(QObject *p, int index, const QByteArray &mp3, bool hasData) {
    setPlayer(p); // AudioPlayer : QMediaPlayer (single inheritance)
    // AUDIO-FIX: stock setPage just called applyVolume(10) (10 % stream gain) and never sets the headset's
    // volume. Full stream gain (local, no D-Bus); the headset level goes once, deferred, per reader.
    applyGain();
    setTrack(index, mp3);
    if (hasData)
        setCover(mp3);
    if (!btRequested) {
        btRequested = true;
        requestBluetooth();
    }
    tick();
}

QString AioAudio::bookKey() const {
    QString k = bookTitle.trimmed();
    k.replace('/', '_');
    k.replace('\\', '_');
    return k;
}

void AioAudio::loadState() {
    savedTrack = -1;
    savedPos = 0;
    durations.clear();
    const QString key = bookKey();
    if (key.isEmpty())
        return;
    QSettings s(AIO_DIR "/positions.ini", QSettings::IniFormat);
    s.beginGroup(key);
    savedTrack = s.value("track", -1).toInt();
    if (savedTrack < 0 || savedTrack > MAX_TRACKS)
        savedTrack = -1;
    savedPos = s.value("pos", 0).toLongLong();
    QStringList ds = s.value("durations").toString().split(' ', QString::SkipEmptyParts);
    for (int i = 0; i < ds.size(); i++) {
        QStringList kv = ds.at(i).split(':');
        bool ok1 = false, ok2 = false;
        if (kv.size() == 2) {
            int idx = kv.at(0).toInt(&ok1);
            qint64 ms = kv.at(1).toLongLong(&ok2);
            if (ok1 && ok2 && idx >= 0 && idx <= MAX_TRACKS && ms > 0)
                durations.insert(idx, ms);
        }
    }
    s.endGroup();
}

// Only on pause, close and track change (never from the 5 s timer).
void AioAudio::saveState(bool askPlayer) {
    const QString key = bookKey();
    if (key.isEmpty() || track < 0 || pendingSeek >= 0)
        return; // a resume not applied yet must not be overwritten
    if (!onboardReady()) {
        nh_log("AIO Audio: /mnt/onboard not mounted, position not saved");
        return;
    }
    qint64 pos = lastPos, dur = lastDur;
    if (askPlayer && player) {
        pos = player->property("position").toLongLong();
        dur = player->property("duration").toLongLong();
    }
    if (pos < 0 || (dur > 0 && pos > dur - 5000))
        pos = 0;
    QStringList ds;
    for (QMap<int, qint64>::const_iterator it = durations.constBegin(); it != durations.constEnd(); ++it)
        ds << QString("%1:%2").arg(it.key()).arg(it.value());
    QSettings s(AIO_DIR "/positions.ini", QSettings::IniFormat);
    s.beginGroup(key);
    s.setValue("track", track);
    s.setValue("pos", pos);
    s.setValue("durations", ds.join(" "));
    s.endGroup();
    s.sync();
    savedTrack = track;
    savedPos = pos;
}

void AioAudio::setTrack(int index, const QByteArray &mp3) {
    const QString album = id3Text(mp3, "TALB");
    const QStringList tr = id3Text(mp3, "TRCK").split('/'); // Kobo AIO: "7/15" over the whole .mp3z
    int no = tr.size() >= 1 ? tr.at(0).trimmed().toInt() : 0;
    int count = tr.size() >= 2 ? tr.at(1).trimmed().toInt() : 0;
    if (no < 1 || no > MAX_TRACKS || count < 0 || count > MAX_TRACKS || (count > 0 && no > count))
        no = count = 0;
    if (!album.isEmpty() && album != bookTitle) {
        bookTitle = album;
        if (titleText)
            titleText->setText(album);
        loadState();
    } else if (album.isEmpty() && bookTitle.isEmpty() && !fallbackTitle.isEmpty()) {
        bookTitle = fallbackTitle; // no TALB: the folder (or file) names the book and its saved position
        if (titleText)
            titleText->setText(bookTitle);
        loadState();
    }
    const int cur = no > 0 ? no - 1 : index;
    const bool first = !started, changed = started && cur != track;
    started = true;
    track = cur;
    trackNo = no > 0 ? no : (index >= 0 ? index + 1 : 0);
    trackCount = count;
    lastPos = 0;
    lastDur = 0;
    footerDirty = true;
    if (first) {
        if (savedTrack == cur && savedPos > 5000) { // libmp3z reopens the track, we bring back the minute
            pendingSeek = savedPos;
            QTimer::singleShot(1000, this, SLOT(applyPending()));
            QTimer::singleShot(2500, this, SLOT(applyPending()));
            QTimer::singleShot(5000, this, SLOT(applyPendingLast()));
        }
    } else if (changed) {
        pendingSeek = -1;
        resumeTarget = -1;
        // One normal (non-DU) refresh of the fast widgets: whatever DU left behind is cleaned once per track.
        normalFlush = true;
        for (int i = 0; i < fastWidgets.size(); i++)
            if (QWidget *w = fastWidgets.at(i).data())
                w->update();
        saveState(false); // new track, from its start
        if (sleepMode == 4 && !(skipClock.isValid() && skipClock.elapsed() < 4000)) {
            pauseOnPlay = true; // "end of track": pause as soon as the next track starts
            QTimer::singleShot(2500, this, SLOT(endOfTrackCheck()));
        }
    }
    if (speedIndex != 0)
        QTimer::singleShot(1500, this, SLOT(applyRate()));
}

void AioAudio::applyPending() {
    if (!player)
        return;
    const qint64 pos = player->property("position").toLongLong();
    if (pendingSeek < 0) {
        // the backend may drop a seek made while it was still loading: repeat it once if we are back at 0
        if (resumeTarget > 15000 && pos < 10000)
            QMetaObject::invokeMethod(player, "setPosition", Q_ARG(qint64, resumeTarget));
        resumeTarget = -1;
        return;
    }
    const qint64 dur = player->property("duration").toLongLong();
    if (dur <= 0)
        return; // not loaded yet: next try, or durationChanged
    if (pendingSeek < dur - 3000) {
        QMetaObject::invokeMethod(player, "setPosition", Q_ARG(qint64, pendingSeek));
        lastPos = pendingSeek;
        resumeTarget = pendingSeek;
    }
    pendingSeek = -1;
    footerDirty = true; // the book percentage jumps with the resumed minute
    QTimer::singleShot(400, this, SLOT(tick()));
}

// Last retry: if the duration is still unknown, seek anyway, so the resume never blocks saveState for good.
void AioAudio::applyPendingLast() {
    if (pendingSeek < 0 || (player && player->property("duration").toLongLong() > 0)) {
        applyPending();
        return;
    }
    if (player) {
        QMetaObject::invokeMethod(player, "setPosition", Q_ARG(qint64, pendingSeek));
        resumeTarget = pendingSeek; // repeated by applyPending once durationChanged arrives, if it was dropped
    }
    lastPos = pendingSeek; // a close before the next tick keeps the saved minute
    pendingSeek = -1;
    footerDirty = true;
    QTimer::singleShot(400, this, SLOT(tick()));
}

void AioAudio::applyRate() {
    if (!player)
        return;
    const qreal want = RATES[speedIndex];
    if (qAbs(player->property("playbackRate").toReal() - want) < 0.01)
        return;
    if (!QMetaObject::invokeMethod(player, "setPlaybackRate", Q_ARG(qreal, want)))
        player->setProperty("playbackRate", QVariant(double(want)));
}

void AioAudio::onDuration() {
    if (!player)
        return;
    const qint64 d = player->property("duration").toLongLong();
    if (d > 0 && track >= 0)
        durations.insert(track, d);
    if (d > 0)
        lastDur = d;
    footerDirty = true; // a new track length: the book total changes
    applyPending();
    tick();
}

bool AioAudio::playPauseText(QLabel *label, const QString &text) {
    if (!playPause || label != playPause.data())
        return false;
    if (text == QLatin1String("Pause"))
        wantPlaying = 1;
    else if (text == QLatin1String("Play"))
        wantPlaying = 0;
    else
        return true; // anything else: keep the icon
    // GStreamer can flip Playing/Paused many times per second when the Bluetooth sink stalls. Each flip used to
    // repaint (colour e-ink), query Bluetooth and write positions.ini, which froze the whole device: settle first.
    // Debounce: every flip restarts the 400 ms wait, but a burst never holds the icon back more than 1.5 s.
    if (!stateTimer->isActive())
        burstClock.start(); // first flip of a burst
    stateTimer->start(burstClock.elapsed() >= 1500 ? 0 : 400); // overdue: next event loop pass, not inside this hook
    return true;
}

void AioAudio::applyPlayState() {
    if (wantPlaying < 0 || !playPause)
        return;
    const bool now = wantPlaying == 1;
    wantPlaying = -1;
    if (now == playing && iconSet)
        return;
    iconSet = true;
    QLabel *label = playPause.data();
    setIcon(label, now ? pauseIcon : playIcon, playBox, now ? pauseIconInv : playIconInv);
    const bool was = playing;
    playing = now;
    tick();
    schedule(); // e-ink: nothing moves while paused
    if (playing) {
        if (pauseOnPlay) {
            pauseOnPlay = false;
            QMetaObject::invokeMethod(this, "sleepFired", Qt::QueuedConnection);
        }
    } else if (was) {
        // A stalling sink pauses and resumes in a loop: one positions.ini write per 15 s at most, and the
        // last pause of a burst is still saved once the 15 s are up (close and track change save directly).
        if (!saveClock.isValid() || saveClock.elapsed() >= 15000) {
            saveTimer->stop();
            savePaused();
        } else if (!saveTimer->isActive()) {
            saveTimer->start(int(qMax<qint64>(0, 15000 - saveClock.elapsed())));
        }
    }
}

void AioAudio::savePaused() {
    if (playing)
        return; // playing again: the next pause saves
    saveClock.start();
    saveState(true);
}

void AioAudio::setChapter(const QString &name) {
    if (chapterText)
        chapterText->setText(dedupe(name)); // the track name only ("Pista k de n" is in the footer)
}

void AioAudio::setCover(const QByteArray &mp3) {
    if (!cover)
        return;
    const int lw = frameWidth();
    QByteArray jpeg = apic(mp3);
    if (jpeg.isEmpty()) {
        const QPixmap *cur = cover->pixmap();
        if (cur && !cur->isNull())
            return;
        drawPlaceholder();
        return;
    }
    // setPage runs before the reader is laid out, so the label size is not known yet: always use the box
    // computed from the screen, which is the cover's fixed size anyway.
    int key = int(qHash(jpeg));
    if (key == coverKey && cover->property("aioCover").toBool())
        return;
    QBuffer buf(&jpeg);
    buf.open(QIODevice::ReadOnly);
    QImageReader jr(&buf);
    QSize native = jr.size();
    if (!native.isValid() || native.width() > 8000 || native.height() > 8000)
        return;
    const QByteArray fmt = jr.format().toLower();
    if (fmt != "jpeg" && fmt != "jpg" && qint64(native.width()) * native.height() > 4000000)
        return; // only JPEG decodes at the scaled size: others would be decoded whole first

    const QSize inner(qMax(1, coverBox.width() - 2 * lw), qMax(1, coverBox.height() - 2 * lw));
    jr.setScaledSize(native.scaled(inner, Qt::KeepAspectRatio)); // the JPEG handler decodes at that scale
    QImage img = jr.read();
    if (img.isNull())
        return;
    cover->setProperty("aioCover", true);
    coverSrc = img;
    placeholder = false;
    cover->setPixmap(framed(img, coverBox, lw));
    coverKey = key;
}

// The 5 s tick and every event that moves the position. Only the elapsed/remaining/slider band changes here;
// the footer at most every 60 s or when a track changed; the sleep label only when its minute changes.
void AioAudio::tick() {
    if (!player || !reader)
        return;
    qint64 pos = player->property("position").toLongLong();
    const qint64 dur = player->property("duration").toLongLong();
    if (pos < 0)
        pos = 0;
    notePos(pos);
    if (dur > 0) {
        lastDur = dur;
        if (track >= 0 && !durations.contains(track))
            durations.insert(track, dur);
    }
    if (tocPane && tocPane->isVisible())
        return; // hidden under the track list: closeToc() ticks again
    if (elapsedText)
        elapsedText->setText(hms(pos));
    if (remainText)
        remainText->setText(dur > 0 ? QString::fromLatin1("-") + hms(qMax<qint64>(0, dur - pos)) : QString());
    if (slider)
        slider->setValue(pos, dur);
    if (footerDirty || !footerClock.isValid() || footerClock.elapsed() >= 60000)
        updateFooter(pos, dur);
    updateSleepLabel();
}

// Whole book: learned track lengths, the average of those for the rest ("≈" until all are known).
void AioAudio::updateFooter(qint64 pos, qint64 dur) {
    footerDirty = false;
    footerClock.start();
    if (!footerText)
        return;
    QString f;
    if (trackNo > 0)
        f = trackCount > 0 ? QString("Pista %1 de %2").arg(trackNo).arg(trackCount) : QString("Pista %1").arg(trackNo);
    if (trackCount > 0 && dur > 0 && track >= 0 && track < trackCount) {
        qint64 known = 0;
        int nk = 0;
        for (QMap<int, qint64>::const_iterator it = durations.constBegin(); it != durations.constEnd(); ++it)
            if (it.key() >= 0 && it.key() < trackCount) {
                known += it.value();
                nk++;
            }
        const qint64 avg = nk ? known / nk : dur;
        qint64 done = pos, all = 0;
        for (int i = 0; i < trackCount; i++) {
            const qint64 d = i == track ? dur : durations.value(i, avg);
            all += d;
            if (i < track)
                done += d;
        }
        if (all > 0) {
            const int pct = int(qBound<qint64>(0, done * 100 / all, 100));
            f += QString::fromUtf8(" · %1% del libro · quedan %2%3")
                     .arg(pct).arg(nk >= trackCount ? QString() : QString::fromUtf8("≈ ")).arg(span(all - done));
        }
    }
    footerText->setText(f);
}

void AioAudio::seekTo(qint64 ms) {
    if (!player)
        return;
    const qint64 dur = player->property("duration").toLongLong();
    if (dur > 0 && ms > dur - 1000)
        ms = dur - 1000;
    if (ms < 0)
        ms = 0;
    pendingSeek = -1; // the user chose a place: no resume over it
    resumeTarget = -1;
    QMetaObject::invokeMethod(player, "setPosition", Q_ARG(qint64, ms));
    lastPos = ms;
    QTimer::singleShot(400, this, SLOT(tick()));
}

void AioAudio::seek(qint64 delta) {
    if (player)
        seekTo(player->property("position").toLongLong() + delta);
}

void AioAudio::rewind() { seek(-30000); }
void AioAudio::ahead() { seek(30000); }
void AioAudio::userSkip() { skipClock.start(); }

// "Mis libros": save, then close like Nickel's own back button (queued: we are inside the tap).
void AioAudio::goHome() {
    if (property("aioClosing").toBool())
        return; // a second tap before the queued close ran
    setProperty("aioClosing", true);
    if (player) {
        lastPos = player->property("position").toLongLong();
        lastDur = player->property("duration").toLongLong();
    }
    saveState(false);
    static QPointer<AioCloser> closer;
    if (!closer)
        closer = new AioCloser(QCoreApplication::instance());
    closer->reader = reader;
    QMetaObject::invokeMethod(closer, "close", Qt::QueuedConnection);
}

void AioCloser::close() {
    QWidget *r = reader.data();
    reader = 0;
    if (!r)
        return; // the reader closed some other way before this ran
    if (MP3Reader_saveBookmark)
        MP3Reader_saveBookmark(r);
    if (ReadingController_closeReadingView)
        ReadingController_closeReadingView(); // static: deletes the reader (and its AioAudio), not this object
}

void AioAudio::cycleSpeed() {
    speedIndex = (speedIndex + 1) % RATE_COUNT; // 1x -> 1,25x -> 1,5x -> 1,75x -> 2x -> 0,8x -> 1x
    if (speedTap)
        speedTap->setText(QString::fromLatin1(RATE_NAMES[speedIndex]));
    applyRate();
}

void AioAudio::cycleSleep() {
    sleepMode = (sleepMode + 1) % 5; // off -> 15 -> 30 -> 60 min -> end of track -> off
    sleepTimer->stop();
    pauseOnPlay = false;
    if (sleepMode >= 1 && sleepMode <= 3) {
        sleepTimer->start(SLEEP_MIN[sleepMode] * 60000);
        sleepClock.start();
    }
    updateSleepLabel();
}

void AioAudio::sleepFired() {
    sleepTimer->stop();
    sleepMode = 0;
    pauseOnPlay = false;
    // The pause goes around MP3Reader, so its "Play" setText may never come: show it ourselves first
    // (icon, `playing`, tick timer, saved position), then pause.
    if (!playPauseText(playPause.data(), QString::fromLatin1("Play"))) {
        playing = false;
        timer->stop();
    }
    if (player)
        QMetaObject::invokeMethod(player, "pause");
    updateSleepLabel();
}

void AioAudio::endOfTrackCheck() {
    if (pauseOnPlay)
        sleepFired();
}

void AioAudio::updateSleepLabel() {
    QString t = QString::fromUtf8("Apagado");
    if (sleepMode >= 1 && sleepMode <= 3) {
        qint64 left = qint64(SLEEP_MIN[sleepMode]) * 60000 - (sleepClock.isValid() ? sleepClock.elapsed() : 0);
        t = QString("%1 min").arg((qMax<qint64>(0, left) + 59999) / 60000);
    } else if (sleepMode == 4) {
        t = QString::fromUtf8("Fin de pista");
    }
    if (sleepTap)
        sleepTap->setText(t); // repaints only when the minute (or the mode) changed
}

// -- volume and Bluetooth -----------------------------------------------------------------------------------

// Bottom bar volume: the headset's own volume (0..127, sent over AVRCP like Kobo's player), shown as a percentage.
void AioAudio::showVolume() {
    if (volText)
        volText->setText(QString::number(volumePercent()) + "%");
}

// A tap only changes the number: the headset gets one setVolume 300 ms after the last tap, the file one write
// 2 s after it.
void AioAudio::stepVolume(int delta) {
    btVolume = qBound(0, btVolumeLoad() + delta, 127);
    showVolume();
    volApplyTimer->start();
    volSaveTimer->start();
}

void AioAudio::volumeDown() { stepVolume(-8); }
void AioAudio::volumeUp() { stepVolume(8); }

void AioAudio::applyVolumeNow() {
    applyGain();
    if (!sendVolume())
        return; // deleted inside libnickel
    if (btDev.isEmpty() && btChecked && !btRetried) { // maybe the headset connected after the book opened
        btRetried = true;
        requestBluetooth();
    }
}

void AioAudio::saveVolumeNow() {
    volSaveTimer->stop();
    if (!onboardReady())
        return;
    QSettings s(AIO_DIR "/positions.ini", QSettings::IniFormat);
    s.setValue("btVolume", btVolumeLoad());
}

// Stream gain (QMediaPlayer, local): full with a headset, our level when there is none. Without the Bluetooth
// API the stock log10 stream volume is left alone.
void AioAudio::applyGain() {
    if (!player || !btVolumeApi())
        return;
    const int gain = btChecked && btDev.isEmpty() ? volumePercent() : 100;
    QMetaObject::invokeMethod(player, "setVolume", Q_ARG(int, gain));
}

// The headset level, only when it differs from what this reader last sent. False: this AioAudio was deleted
// during the libnickel call (nothing of it may be touched).
bool AioAudio::sendVolume() {
    if (btDev.isEmpty() || !BluetoothManager_sharedInstance || !BluetoothManager_setVolume)
        return true;
    const int v = btVolumeLoad();
    if (v == sentVolume)
        return true;
    QPointer<AioAudio> self(this);
    const QString dev = btDev; // a copy: the member may change while libnickel runs
    void *bm = BluetoothManager_sharedInstance();
    if (!self)
        return false;
    if (!bm)
        return true;
    BluetoothManager_setVolume(bm, dev, (unsigned char)v, (unsigned char)v);
    if (!self)
        return false;
    sentVolume = v;
    nh_log("AIO Audio: headset volume %d/127", v);
    return true;
}

// One deferred read at a time across readers (btQueued, held until that read ends). While another reader's read
// is queued or running, retry every second, at most 5 times.
void AioAudio::requestBluetooth() {
    if (!btVolumeApi())
        return;
    if (btQueued) {
        if (!btMine && !btRetryPending && btTries < 5) {
            btRetryPending = true;
            btTries++;
            QTimer::singleShot(1000, this, SLOT(retryBluetooth()));
        }
        return; // ours is already queued or running
    }
    btQueued = true;
    btMine = true;
    btTries = 0;
    QTimer::singleShot(0, this, SLOT(refreshBluetooth())); // after the current event: never inside a tap or setMedia
}

void AioAudio::retryBluetooth() {
    btRetryPending = false;
    requestBluetooth();
}

// The deferred Bluetooth read. btQueued stays set until it ends, so nothing (a tap, another reader) starts a second
// read from an event loop that libnickel might run inside one of its calls.
void AioAudio::refreshBluetooth() {
    static bool inFlight = false;
    if (inFlight)
        return; // re-entered: the running read releases btQueued
    inFlight = true;
    const bool alive = readBluetooth();
    inFlight = false;
    if (!alive)
        return; // ~AioAudio already released btQueued
    btQueued = false;
    btMine = false;
}

// Sink path, software volume and unmute (once per device per Nickel run), our level (only if it changed) and
// the headset name. After every libnickel call: stop if this AioAudio was deleted meanwhile. The device path is
// a local copy. Never logs the headset name or address.
bool AioAudio::readBluetooth() {
    QPointer<AioAudio> self(this);
    void *bm = BluetoothManager_sharedInstance ? BluetoothManager_sharedInstance() : 0;
    if (!self)
        return false;
    if (!bm || !BluetoothManager_a2dpSink)
        return true;
    const QString dev = BluetoothManager_a2dpSink(bm);
    if (!self)
        return false;
    btChecked = true;
    if (dev != btDev) {
        btDev = dev;
        btName.clear();
        sentVolume = -1;
    }
    if (dev.isEmpty()) {
        nh_log("AIO Audio: no A2DP headset connected");
    } else {
        if (!btPrepared.contains(dev)) {
            btPrepared << dev;
            bool absolute = true;
            if (BluetoothManager_supportsAbsoluteVolume) {
                absolute = BluetoothManager_supportsAbsoluteVolume(bm, dev);
                if (!self)
                    return false;
            }
            if (BluetoothManager_setSoftwareVolume) {
                BluetoothManager_setSoftwareVolume(bm, dev, !absolute);
                if (!self)
                    return false;
            }
            if (BluetoothManager_setMuted) {
                BluetoothManager_setMuted(bm, dev, false, false);
                if (!self)
                    return false;
            }
            nh_log("AIO Audio: headset prepared (%s volume)", absolute ? "absolute" : "software");
        }
        if (!sendVolume())
            return false;
        const QString name = headsetName(dev);
        if (!self)
            return false;
        if (btDev == dev)
            btName = name;
    }
    applyGain();
    updateBtChip();
    return true;
}

// The Bluetooth chip only takes the tap; at most one deferred re-read per reader (a headset that connected later).
void AioAudio::btTapped() {
    if (btRetried)
        return;
    btRetried = true;
    requestBluetooth();
}

void AioAudio::updateBtChip() {
    if (btTap)
        btTap->setText(btName.isEmpty() ? QString::fromLatin1("Bluetooth") : btName);
}

// -- hooks ----------------------------------------------------------------------------------------------

extern "C" __attribute__((visibility("default"))) void _aio_reader(void *self, QWidget *parent, void *state) {
    const bool outer = inReaderCtor;
    inReaderCtor = true; // setMedia calls from here on are kept for the replay below
    MP3Reader_MP3Reader(self, parent, state);
    inReaderCtor = outer;
    // MP3Reader passes `this` to Ui_MP3Reader::setupUi as the form QWidget, so the QObject is at offset 0.
    QWidget *reader = reinterpret_cast<QWidget *>(self);
    if (qstrcmp(reader->metaObject()->className(), "MP3Reader") || reader->objectName() != "MP3Reader") {
        nh_log("unexpected reader %s, keeping the stock layout", reader->metaObject()->className());
        forgetMedia();
        return;
    }
    aio = new AioAudio(reader);
    // Stock setPage may already have called setMedia inside the constructor, before AioAudio existed.
    if (lastMediaPlayer && aio->owns(lastMediaPlayer.data()))
        aio->media(lastMediaPlayer.data(), lastMediaIndex, lastMediaData, lastMediaHasData);
    else if (lastMediaPlayer)
        nh_log("AIO Audio: pending track is not this reader's player, dropped");
    forgetMedia();
}

extern "C" __attribute__((visibility("default"))) void _aio_setText(QLabel *self, const QString &text) {
    if (!self) {
        QLabel_setText(self, text);
        return;
    }
    if (aio && aio->playPauseText(self, text))
        return; // play/pause icon (no Bluetooth call here: play/pause changes never touch BluetoothManager)
    if (self->property("aioOwned").toBool())
        return; // a stock button we restyled: keep our icon/text
    const QString t = translate(self, text, !aio.isNull());
    if (self->objectName() == QLatin1String("title")) {
        self->setProperty("aioName", t); // read by AioAudio's constructor when setPage ran before it
        if (aio)
            aio->setChapter(t);
    }
    QLabel_setText(self, t);
}

extern "C" __attribute__((visibility("default"))) void _aio_setPixmap(QLabel *self, const QPixmap &pixmap) {
    if (self && self->property("aioCover").toBool())
        return; // our cover, read from the track itself, stays
    if (self && aio && !pixmap.isNull() && self->property("aioCoverSlot").toBool()) {
        QLabel_setPixmap(self, aio->framedCover(pixmap)); // the stock cover, fitted into our framed box
        return;
    }
    QLabel_setPixmap(self, pixmap);
}

// The stock cover lambda moves coverImage to (0,0) at the picture's native size before setPixmap.
extern "C" __attribute__((visibility("default"))) void _aio_setGeometry(QWidget *self, const QRect &rect) {
    if (self && (self->property("aioCoverSlot").toBool() || self->property("aioCover").toBool()))
        return;
    QWidget_setGeometry(self, rect);
}

extern "C" __attribute__((visibility("default"))) void _aio_setMedia(void *self, int index, const void *content, QIODevice *device) {
    AudioPlayer_setMedia(self, index, content, device);
    QObject *player = reinterpret_cast<QObject *>(self); // AudioPlayer : QMediaPlayer (single inheritance)
    QBuffer *buffer = qobject_cast<QBuffer *>(device);
    const QByteArray data = buffer ? buffer->data() : QByteArray();
    if (aio && aio->owns(player)) {
        forgetMedia();
        aio->media(player, index, data, buffer != 0); // stream gain now, headset level deferred
        return;
    }
    if (!inReaderCtor) {
        // Not inside an MP3Reader constructor: nobody would replay it, so the track is not kept in memory.
        if (aio)
            nh_log("AIO Audio: track for a player without its AioAudio, not kept");
        forgetMedia();
        return;
    }
    // No AioAudio for this reader yet (setPage inside the MP3Reader constructor): _aio_reader replays it.
    lastMediaPlayer = player;
    lastMediaIndex = index;
    lastMediaData = data;
    lastMediaHasData = buffer != 0;
}

// -- navigation: track changes only from our controls ------------------------------------------------------

// A libmp3z navigation call to drop: our layout is on for this reader, the call comes from
// MP3Reader::qt_static_metacall (Nickel's reading view: taps, swipes and page keys on the cover and the edges)
// and the track is not over (auto-advance). Our hooks are reached through libmp3z's PLT stub, which does not touch
// lr, so our return address is the caller's.
static bool blockNav(void *self, void *ret, const char *what) {
    if (!navRangeOk || !aio || !aio->restyled() || static_cast<void *>(aio->readerWidget()) != self)
        return false;
    const uintptr_t pc = reinterpret_cast<uintptr_t>(ret) & ~uintptr_t(1);
    if (pc <= metacallStart || pc >= metacallEnd)
        return false; // stock prev/next buttons (PMF), headset media keys (keyPressEvent), anything else
    if (aio->trackEnded())
        return false;
    static bool logged = false;
    if (!logged) {
        logged = true;
        nh_log("AIO Audio: %s from Nickel's reading view dropped (track changes only from the player)", what);
    }
    return true;
}

extern "C" __attribute__((visibility("default"))) void _aio_nextPage(void *self) {
    if (blockNav(self, __builtin_return_address(0), "goToNextPage"))
        return;
    MP3Reader_goToNextPage(self);
}

extern "C" __attribute__((visibility("default"))) void _aio_prevPage(void *self) {
    if (blockNav(self, __builtin_return_address(0), "goToPrevPage"))
        return;
    MP3Reader_goToPrevPage(self);
}

extern "C" __attribute__((visibility("default"))) void _aio_goToPage(void *self, int page) {
    if (blockNav(self, __builtin_return_address(0), "goToPage"))
        return;
    MP3Reader_goToPage(self, page);
}

extern "C" __attribute__((visibility("default"))) void _aio_nextChapter(void *self) {
    if (blockNav(self, __builtin_return_address(0), "goToNextChapter"))
        return;
    MP3Reader_goToNextChapter(self);
}

extern "C" __attribute__((visibility("default"))) void _aio_prevChapter(void *self, int how) {
    if (blockNav(self, __builtin_return_address(0), "goToPrevChapter"))
        return;
    MP3Reader_goToPrevChapter(self, how);
}

// The ReadingView that shows our player (its reader at ReadingView+0x4c, 4.45 RE: keyPressEvent posts media keys
// there), while it is shown. Anything else: Nickel's own handling.
static bool ownView(void *view) {
    if (!view || !ReadingController_closeReadingView || !aio || !aio->restyled() || !aio->readerShown())
        return false; // without our "Mis libros" link, Nickel's reading menu is the way out
    const QWidget *r = *reinterpret_cast<QWidget *const *>(static_cast<const char *>(view) + 0x4c);
    return r && r == aio->readerWidget();
}

extern "C" __attribute__((visibility("default"))) void _aio_viewTap(void *self, void *tap) {
    if (ownView(self)) {
        static bool logged = false;
        if (!logged) {
            logged = true;
            nh_log("AIO Audio: reading view tap over the player swallowed (no menu, no page turn)");
        }
        return;
    }
    ReadingView_processTap(self, tap);
}

extern "C" __attribute__((visibility("default"))) void _aio_viewSwipe(void *self, void *swipe) {
    if (ownView(self))
        return;
    ReadingView_processSwipe(self, swipe);
}

// -- fast refresh ---------------------------------------------------------------------------------------------

// Kobo's QtWidgets flushes with an empty list (libkobo: default, colour-aware waveform). When only our black and
// white controls of the reader's window changed, ask for one DU partial update of the region's bounding rect (one
// pair, like Kobo's own flush). Every other flush is untouched, and nothing is allocated unless all the conditions
// hold.
extern "C" __attribute__((visibility("default"))) void _aio_flush(QBackingStore *self, const QRegion &region,
                                                                  QWindow *win, const QPoint &offset,
                                                                  const AioFlushList &list) {
    if (fastFlushOn && !self) {
        fastFlushOn = false;
        nh_log("AIO Audio: QBackingStore::flush without a backing store, fast refresh off");
    }
    if (fastFlushOn && win && list.isEmpty() && aio && aio->fastRegion(region, win)) {
        AioFlushList du;
        du.append(qMakePair(region.boundingRect(), quint64(AIO_WAVE_DU_PARTIAL)));
        static bool logged = false;
        if (!logged) {
            logged = true;
            nh_log("AIO Audio: fast refresh (DU) for the player controls");
        }
        QBackingStore_flush(self, region, win, offset, du);
        return;
    }
    QBackingStore_flush(self, region, win, offset, list);
}

static int aio_init() {
    nh_log("AIO Audio ready");
    return 0;
}

static struct nh_info AioAudioInfo = {
    .name            = "AIO Audio",
    .desc            = "Reproductor de audiolibros .mp3z más claro (Kobo AIO)",
    .uninstall_flag  = AIO_DIR "/uninstall",
    .uninstall_xflag = NULL,
    .failsafe_delay  = 3,
};

static struct nh_hook AioAudioHook[] = {
    {.sym = "_ZN9MP3ReaderC1EP7QWidgetP11PluginState", .sym_new = "_aio_reader", .lib = MP3Z, .out = nh_symoutptr(MP3Reader_MP3Reader), .desc = "player layout", .optional = true},
    {.sym = "_ZN6QLabel7setTextERK7QString", .sym_new = "_aio_setText", .lib = MP3Z, .out = nh_symoutptr(QLabel_setText), .desc = "Spanish labels", .optional = true},
    {.sym = "_ZN6QLabel9setPixmapERK7QPixmap", .sym_new = "_aio_setPixmap", .lib = MP3Z, .out = nh_symoutptr(QLabel_setPixmap), .desc = "keep cover", .optional = true},
    {.sym = "_ZN7QWidget11setGeometryERK5QRect", .sym_new = "_aio_setGeometry", .lib = MP3Z, .out = nh_symoutptr(QWidget_setGeometry), .desc = "keep cover place", .optional = true},
    {.sym = "_ZN11AudioPlayer8setMediaEiRK13QMediaContentP9QIODevice", .sym_new = "_aio_setMedia", .lib = MP3Z, .out = nh_symoutptr(AudioPlayer_setMedia), .desc = "cover and seeking", .optional = true},
    {.sym = "_ZN9MP3Reader12goToNextPageEv", .sym_new = "_aio_nextPage", .lib = MP3Z, .out = nh_symoutptr(MP3Reader_goToNextPage), .desc = "tracks only from the player", .optional = true},
    {.sym = "_ZN9MP3Reader12goToPrevPageEv", .sym_new = "_aio_prevPage", .lib = MP3Z, .out = nh_symoutptr(MP3Reader_goToPrevPage), .desc = "tracks only from the player", .optional = true},
    {.sym = "_ZN9MP3Reader8goToPageEi", .sym_new = "_aio_goToPage", .lib = MP3Z, .out = nh_symoutptr(MP3Reader_goToPage), .desc = "tracks only from the player", .optional = true},
    {.sym = "_ZN9MP3Reader15goToNextChapterEv", .sym_new = "_aio_nextChapter", .lib = MP3Z, .out = nh_symoutptr(MP3Reader_goToNextChapter), .desc = "tracks only from the player", .optional = true},
    {.sym = "_ZN9MP3Reader15goToPrevChapterE21ReadingNavigationType", .sym_new = "_aio_prevChapter", .lib = MP3Z, .out = nh_symoutptr(MP3Reader_goToPrevChapter), .desc = "tracks only from the player", .optional = true},
    {.sym = "_ZN11ReadingView10processTapEP10TapGesture", .sym_new = "_aio_viewTap", .lib = "libnickel.so.1.0.0", .out = nh_symoutptr(ReadingView_processTap), .desc = "no reading menu over the player", .optional = true},
    {.sym = "_ZN11ReadingView12processSwipeEP12SwipeGesture", .sym_new = "_aio_viewSwipe", .lib = "libnickel.so.1.0.0", .out = nh_symoutptr(ReadingView_processSwipe), .desc = "no page turn over the player", .optional = true},
    {.sym = "_ZN13QBackingStore5flushERK7QRegionP7QWindowRK6QPointRK5QListI5QPairI5QRectyEE", .sym_new = "_aio_flush", .lib = "libQt5Widgets.so.5", .out = nh_symoutptr(QBackingStore_flush), .desc = "fast refresh of the player controls", .optional = true},
    {0},
};

static struct nh_dlsym AioAudioDlsym[] = {
    {.name = "_ZN10TouchLabel19setDeselectedPixmapERK7QPixmap", .out = nh_symoutptr(TouchLabel_setDeselectedPixmap), .desc = "tap feedback (normal)", .optional = true},
    {.name = "_ZN10TouchLabel17setSelectedPixmapERK7QPixmap", .out = nh_symoutptr(TouchLabel_setSelectedPixmap), .desc = "tap feedback (pressed)", .optional = true},
    {.name = "_ZN17ReadingController16closeReadingViewEv", .out = nh_symoutptr(ReadingController_closeReadingView), .desc = "back to My Books", .optional = true},
    {.name = "_ZNK7Content5getIdEv", .out = nh_symoutptr(Content_getId), .desc = "book path (title, track list)", .optional = true},
    {.name = "_ZN16BluetoothManager14sharedInstanceEv", .out = nh_symoutptr(BluetoothManager_sharedInstance), .desc = "headset volume", .optional = true},
    {.name = "_ZNK16BluetoothManager8a2dpSinkEv", .out = nh_symoutptr(BluetoothManager_a2dpSink), .desc = "headset volume", .optional = true},
    {.name = "_ZNK16BluetoothManager22supportsAbsoluteVolumeERK7QString", .out = nh_symoutptr(BluetoothManager_supportsAbsoluteVolume), .desc = "headset volume", .optional = true},
    {.name = "_ZN16BluetoothManager17setSoftwareVolumeERK7QStringb", .out = nh_symoutptr(BluetoothManager_setSoftwareVolume), .desc = "headset volume", .optional = true},
    {.name = "_ZN16BluetoothManager8setMutedERK7QStringbb", .out = nh_symoutptr(BluetoothManager_setMuted), .desc = "headset volume", .optional = true},
    {.name = "_ZN16BluetoothManager9setVolumeERK7QStringhh", .out = nh_symoutptr(BluetoothManager_setVolume), .desc = "headset volume", .optional = true},
    {.name = "_ZN15BluetoothDeviceC1ERK7QString", .out = nh_symoutptr(BluetoothDevice_BluetoothDevice), .desc = "headset name", .optional = true},
    {.name = "_ZNK15BluetoothDevice11displayNameEv", .out = nh_symoutptr(BluetoothDevice_displayName), .desc = "headset name", .optional = true},
    {.name = "_ZN15BluetoothDeviceD1Ev", .out = nh_symoutptr(BluetoothDevice_destroy), .desc = "headset name", .optional = true},
    {0},
};

NickelHook(
    .init  = aio_init,
    .info  = &AioAudioInfo,
    .hook  = AioAudioHook,
    .dlsym = AioAudioDlsym,
)
