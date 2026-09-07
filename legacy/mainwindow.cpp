#include "mainwindow.h"
#include "sq1widget.h"
#include "sq1-core/sq1-logic.h"
#include "sq1-core/karnotation.h"
#include "styles/theme.h"
#include "sq1-core/output-converter.h"
#include "sq1-core/sq1opt-runner.h"
#include "styles/stylesheet.h"
#include <QApplication>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QGridLayout>
#include <QCheckBox>
#include <QLineEdit>
#include <QStyledItemDelegate>
#include <QTextEdit>
#include <QPushButton>
#include <QLabel>
#include <QScrollArea>
#include <QSpinBox>
#include <QProgressBar>
#include <QGroupBox>
#include <QButtonGroup>
#include <QClipboard>
#include <QSet>
#include <QCoreApplication>
#include <QProxyStyle>
#include <QToolTip>
#include <QHelpEvent>
#include <QTextBlock>
#include <QScrollBar>
#include <QMenu>
#include <QTimer>
#include <QPropertyAnimation>
#include <QGraphicsOpacityEffect>
#include <QDialog>
#include <QShortcut>
#include <QSettings>
#include <QPlainTextEdit>
#include <QDateTime>
#include <QTextBrowser>
#include <QString>
#include <QDir>
#include <QUrl>
#include <QDesktopServices>
#include <QFile>
#include <QDir>
#include <QRegularExpression>
#include <QRegularExpressionValidator>
#include <QDebug>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <exception>
#include <iostream>
#include <mutex>
#include <sstream>
#include <streambuf>
#include <vector>
#include <string>
#include <map>
#include <set>
#include <QPainter>
#include <QGraphicsView>
#include <QGraphicsScene>
#include <QGraphicsProxyWidget>

// ============================================================
// TightCheckBox — only shows tooltip when hovering over indicator+text
// ============================================================
class TightCheckBox : public QCheckBox
{
public:
    using QCheckBox::QCheckBox;
    bool event(QEvent *e) override
    {
        if (e->type() == QEvent::ToolTip)
        {
            QStyleOptionButton opt;
            opt.initFrom(this);
            QRect textRect = style()->subElementRect(QStyle::SE_CheckBoxContents, &opt, this);
            QRect indRect = style()->subElementRect(QStyle::SE_CheckBoxIndicator, &opt, this);
            QRect activeRect = textRect.united(indRect);
            QHelpEvent *he = static_cast<QHelpEvent *>(e);
            if (!activeRect.contains(he->pos()))
                return true; // swallow
        }
        return QCheckBox::event(e);
    }
};

// ============================================================
// SelectableDelegate — makes table cells selectable by mouse drag
// ============================================================
class SelectableDelegate : public QStyledItemDelegate
{
public:
    explicit SelectableDelegate(QObject *parent = nullptr) : QStyledItemDelegate(parent) {}
    QWidget *createEditor(QWidget *parent, const QStyleOptionViewItem &, const QModelIndex &) const override
    {
        QLineEdit *ed = new QLineEdit(parent);
        ed->setReadOnly(true);
        ed->setFrame(false);
        ed->setStyleSheet("QLineEdit { background: transparent; border: none; padding: 0 4px; selection-background-color: #3a6ea8; selection-color: #ffffff; }");
        ed->setCursor(Qt::IBeamCursor);
        return ed;
    }
    void setEditorData(QWidget *editor, const QModelIndex &index) const override
    {
        static_cast<QLineEdit *>(editor)->setText(index.data().toString());
    }
    void setModelData(QWidget *, QAbstractItemModel *, const QModelIndex &) const override {}
    void updateEditorGeometry(QWidget *editor, const QStyleOptionViewItem &option, const QModelIndex &) const override
    {
        editor->setGeometry(option.rect);
    }
};

// ============================================================
// FadingTooltip — singleton tooltip with fade-in, shown via hover timer
// ============================================================
class FadingTooltip : public QWidget
{
public:
    static void arm(const QString &text, const QPoint &globalPos, QWidget *parent)
    {
        FadingTooltip &ft = inst(parent);
        ft.m_buttonMode = false;
        ft.armImpl(text, globalPos);
    }
    // Arm anchored to a button's actual screen rect (computed via QGraphicsView transform).
    // Tooltip will appear centered below the button, not at the cursor.
    static void armButton(const QString &text, const QRect &globalRect, QWidget *parent)
    {
        inst(parent).armButtonImpl(text, globalRect);
    }
    static void dismiss(QWidget *parent)
    {
        inst(parent).dismissImpl();
    }
    static bool active(QWidget *parent)
    {
        return inst(parent).isVisible();
    }
    static void dismissNow(QWidget *parent)
    {
        inst(parent).dismissNowImpl();
    }

private:
    explicit FadingTooltip(QWidget *parent) : QWidget(parent, Qt::SubWindow)
    {
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setAttribute(Qt::WA_NoSystemBackground);
        setAutoFillBackground(false);

        m_label = new QLabel(this);
        m_label->setWordWrap(false);
        m_label->setContentsMargins(8, 5, 8, 5);

        QVBoxLayout *l = new QVBoxLayout(this);
        l->setContentsMargins(0, 0, 0, 0);
        l->addWidget(m_label);

        m_effect = new QGraphicsOpacityEffect(this);
        setGraphicsEffect(m_effect);
        m_effect->setOpacity(0.0);

        m_hoverTimer = new QTimer(this);
        m_hoverTimer->setSingleShot(true);
        connect(m_hoverTimer, &QTimer::timeout, this, &FadingTooltip::showNow);

        m_closeTimer = new QTimer(this);
        m_closeTimer->setSingleShot(true);
        connect(m_closeTimer, &QTimer::timeout, this, &FadingTooltip::dismissImpl);

        m_dismissTimer = new QTimer(this);
        m_dismissTimer->setSingleShot(true);
        connect(m_dismissTimer, &QTimer::timeout, this, [this]
                {
            m_currentText.clear();
            m_effect->setOpacity(0.0);
            hide(); });

        hide();
    }

    static FadingTooltip &inst(QWidget *parent)
    {
        static QPointer<FadingTooltip> s_inst;
        if (!s_inst)
            s_inst = new FadingTooltip(parent);
        return *s_inst;
    }

    void stopAnim()
    {
        if (m_anim)
        {
            m_anim->stop();
            m_anim = nullptr;
        }
    }

    void armButtonImpl(const QString &text, const QRect &globalRect)
    {
        m_buttonMode = true;
        m_btnGlobalRect = globalRect;
        // Use rect center as pendingPos so the manhattan distance guard still works
        armImpl(text, globalRect.center());
    }

    void armImpl(const QString &text, const QPoint &globalPos)
    {
        m_pendingText = text;
        m_pendingPos = globalPos;

        // Increment generation so any pending dismiss-retry is cancelled — a new
        // arm() means the cursor moved to a new tooltip and the old retry is stale.
        ++m_armGen;

        // Flag stray dismisses from QGraphicsView scene/hover processing: they fire
        // immediately after arm() for WA_Hover widgets and would cancel a valid show.
        m_armJustFired = true;
        QTimer::singleShot(0, this, [this]
                           { m_armJustFired = false; });

        // If a dismiss is pending, cancel it — we're still in tooltip territory
        bool gracePending = m_dismissTimer->isActive();
        if (gracePending)
            m_dismissTimer->stop();

        // Same text already visible (or grace-pending with same text) — just reposition
        if ((isVisible() || gracePending) && m_currentText == text)
        {
            if (!isVisible())
            {
                m_effect->setOpacity(1.0);
                show();
            }
            showNow();
            return;
        }
        // Different text visible/grace-pending — update immediately without delay
        if (isVisible() || gracePending)
        {
            stopAnim();
            m_hoverTimer->stop();
            showNow();
            return;
        }
        // Not yet visible: start hover delay, updating pos each move
        m_hoverTimer->start(200);
    }

    void dismissImpl()
    {
        // Stray dismiss from QGraphicsView scene/hover processing fires right after
        // arm() for WA_Hover widgets. Ignore it — arm already set up the correct state.
        if (m_armJustFired)
        {
            // This dismiss fired in the same event-loop tick as arm() — it's a stray
            // from QGraphicsView hover processing, not a genuine leave. Schedule a
            // one-tick retry: by then m_armJustFired is cleared. If the cursor
            // genuinely left the widget no new arm() will have fired, so the retry
            // will proceed. If a new arm() did fire (cursor moved to another tooltip),
            // the generation counter mismatch cancels the retry.
            int gen = m_armGen;
            QTimer::singleShot(0, this, [this, gen]
                               {
                if (m_armGen == gen)
                    dismissImpl(); });
            return;
        }
        m_closeTimer->stop();
        if (!isVisible())
        {
            // Don't stop the hover timer here for the same reason as m_armJustFired:
            // a stray dismiss would kill the pending show. showNow() guards with the
            // manhattan-distance check before actually showing anything.
            return;
        }
        m_hoverTimer->stop();
        stopAnim();
        // Short grace period: if arm() fires within this window the tooltip
        // updates instantly without flickering through a hidden state.
        // Guard the restart: MouseMove events fire every ~16ms so calling
        // start() on every event would perpetually reset the countdown and
        // the tooltip would never hide during continuous mouse movement.
        if (!m_dismissTimer->isActive())
            m_dismissTimer->start(750);
    }

    void dismissNowImpl()
    {
        m_hoverTimer->stop();
        m_closeTimer->stop();
        m_dismissTimer->stop();
        if (!isVisible())
            return;
        stopAnim();
        m_currentText.clear();
        m_effect->setOpacity(0.0);
        hide();
    }

    void applyThemeStyle()
    {
        m_cachedBg = Theme::fadingTooltipBg();
        m_cachedBorder = Theme::fadingTooltipBorder();
        m_label->setStyleSheet(QString(
                                   "QLabel { background: transparent; color: %1; font-size: 11px; }")
                                   .arg(Theme::fadingTooltipText()));
        setStyleSheet(QString(
                          "FadingTooltip { background: %1; border: 1px solid %2; border-radius: 5px; }")
                          .arg(m_cachedBg, m_cachedBorder));
        update();
    }

    void showNow()
    {
        // If the cursor moved more than ~40px from where arm() last fired, the
        // user left the tooltip widget before the timer fired — skip showing.
        if ((QCursor::pos() - m_pendingPos).manhattanLength() > 80)
            return;

        bool reposition = !isVisible() || m_currentText != m_pendingText;

        m_currentText = m_pendingText;
        applyThemeStyle();
        m_label->setText(m_currentText);
        m_label->adjustSize();
        adjustSize();

        if (reposition)
        {
            QWidget *win = parentWidget();
            int x, y;

            if (m_buttonMode && !m_btnGlobalRect.isNull())
            {
                // Anchor to button geometry: centered below, 5px gap; flip above if needed.
                QPoint btnTL = win->mapFromGlobal(m_btnGlobalRect.topLeft());
                QPoint btnBR = win->mapFromGlobal(m_btnGlobalRect.bottomRight());
                x = (btnTL.x() + btnBR.x()) / 2 - width() / 2;
                y = (btnBR.y() + 5 + height() + 4 <= win->height())
                        ? btnBR.y() + 5
                        : btnTL.y() - height() - 5;
            }
            else
            {
                QPoint cur = win->mapFromGlobal(m_pendingPos);

                // Prefer below-right; flip above if it would clip the bottom edge
                x = cur.x() + 20;
                y = (cur.y() + 28 + height() + 8 <= win->height())
                        ? cur.y() + 28
                        : cur.y() - height() - 8;

                // Flip left if it would clip the right edge
                if (x + width() + 8 > win->width())
                    x = cur.x() - width() - 4;
            }

            // Final clamp so it always stays inside the window
            x = qBound(4, x, win->width() - width() - 4);
            y = qBound(4, y, win->height() - height() - 4);
            move(x, y);
        }

        raise();
        show();

        // Fade in from current opacity so switching tooltips never resets to 0
        stopAnim();
        double startOpacity = m_effect->opacity();
        QPropertyAnimation *anim = new QPropertyAnimation(m_effect, "opacity", this);
        m_anim = anim;
        anim->setDuration(static_cast<int>(160 * (1.0 - startOpacity)));
        anim->setStartValue(startOpacity);
        anim->setEndValue(1.0);
        anim->setEasingCurve(QEasingCurve::OutCubic);
        connect(anim, &QPropertyAnimation::finished, this, [this, anim]
                {
            if (m_anim == anim) m_anim = nullptr; });
        anim->start(QAbstractAnimation::DeleteWhenStopped);

        m_closeTimer->start(8000);
    }

    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.setBrush(QColor(m_cachedBg.isEmpty() ? Theme::fadingTooltipBg() : m_cachedBg));
        p.setPen(QColor(m_cachedBorder.isEmpty() ? Theme::fadingTooltipBorder() : m_cachedBorder));
        p.drawRoundedRect(rect().adjusted(0, 0, -1, -1), 5, 5);
    }

    QLabel *m_label{nullptr};
    QGraphicsOpacityEffect *m_effect{nullptr};
    QTimer *m_hoverTimer{nullptr};
    QTimer *m_closeTimer{nullptr};
    QTimer *m_dismissTimer{nullptr};
    QPropertyAnimation *m_anim{nullptr};
    bool m_armJustFired{false};
    int m_armGen{0};
    bool m_buttonMode{false};
    QRect m_btnGlobalRect;
    QString m_pendingText;
    QPoint m_pendingPos;
    QString m_currentText;
    QString m_cachedBg;
    QString m_cachedBorder;
};

// ============================================================
// FastTipStyle — QProxyStyle that makes tooltips appear instantly.
// Also extends the fall-asleep delay so tooltips linger naturally.
// ============================================================
class FastTipStyle : public QProxyStyle
{
public:
    using QProxyStyle::QProxyStyle;
    int styleHint(StyleHint hint, const QStyleOption *opt = nullptr,
                  const QWidget *widget = nullptr,
                  QStyleHintReturn *ret = nullptr) const override
    {
        if (hint == QStyle::SH_ToolTip_WakeUpDelay)
            return 350;
        if (hint == QStyle::SH_ToolTip_FallAsleepDelay)
            return 8000; // linger 8 s
        return QProxyStyle::styleHint(hint, opt, widget, ret);
    }
};

// -------------------------------------------------------
// SolverWorker
// -------------------------------------------------------
namespace
{
    std::mutex g_solverStreamMutex;

    class SolverStreamBuffer : public std::streambuf
    {
    public:
        explicit SolverStreamBuffer(SolverWorker *worker) : m_worker(worker) {}

        ~SolverStreamBuffer() override
        {
            flushLine();
        }

    protected:
        int overflow(int ch) override
        {
            if (ch == traits_type::eof())
                return traits_type::not_eof(ch);

            char c = static_cast<char>(ch);
            if (c == '\n')
                flushLine();
            else if (c != '\r')
                m_line.push_back(c);
            return ch;
        }

        std::streamsize xsputn(const char *s, std::streamsize count) override
        {
            for (std::streamsize i = 0; i < count; ++i)
                overflow(static_cast<unsigned char>(s[i]));
            return count;
        }

        int sync() override
        {
            flushLine();
            return 0;
        }

    private:
        void flushLine()
        {
            if (m_line.empty())
                return;
            emit m_worker->lineReady(QString::fromStdString(m_line).trimmed());
            m_line.clear();
        }

        SolverWorker *m_worker;
        std::string m_line;
    };
}

void SolverWorker::requestStop()
{
    m_stopRequested.store(true);
    sq1optRequestStop();
    emit lineReady("Stop requested. The integrated solver will stop when the current solve returns.");
}

void SolverWorker::run()
{
    std::lock_guard<std::mutex> streamLock(g_solverStreamMutex);
    m_stopRequested.store(false);

    QDir tableDir(QCoreApplication::applicationDirPath());
    if (!tableDir.exists("pruning-tables"))
        tableDir.mkpath("pruning-tables");
    tableDir.cd("pruning-tables");
    sq1optSetTableDirectory(tableDir.absolutePath().toStdString());

    QStringList args;
    args << "sq1opt";
    args << "-v5";
    args.append(flags);
    args << positionStr;

    std::vector<std::string> argStorage;
    std::vector<char *> argv;
    argStorage.reserve(args.size());
    argv.reserve(args.size());
    for (const QString &arg : args)
    {
        argStorage.push_back(arg.toStdString());
        argv.push_back(argStorage.back().data());
    }

    SolverStreamBuffer outBuffer(this);
    std::streambuf *oldOut = std::cout.rdbuf(&outBuffer);
    std::streambuf *oldErr = std::cerr.rdbuf(&outBuffer);
    int exitCode = -1;
    try
    {
        exitCode = sq1optMain(static_cast<int>(argv.size()), argv.data());
    }
    catch (const std::exception &e)
    {
        emit lineReady(QString("ERROR: %1").arg(e.what()));
        exitCode = -1;
    }
    catch (...)
    {
        emit lineReady("ERROR: Unknown solver failure.");
        exitCode = -1;
    }

    std::cout.flush();
    std::cerr.flush();
    outBuffer.pubsync();
    std::cout.rdbuf(oldOut);
    std::cerr.rdbuf(oldErr);
    emit finished(exitCode);
}

// -------------------------------------------------------
// MainWindow
// -------------------------------------------------------
class AlgBlockData : public QTextBlockUserData
{
public:
    QString rawLine;
    explicit AlgBlockData(const QString &r) : rawLine(r) {}
};

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent)
{
    setWindowTitle("Croissant");
    setMinimumSize(720, 560);
    resize(860, 700);

    // ── Instant tooltips ──────────────────────────────────────────────────────
    // Capture the current style name so FastTipStyle wraps a fresh copy of the
    // same base style, avoiding ownership issues with the old style pointer.
    {
        QString base = QApplication::style()->objectName();
        QApplication::setStyle(new FastTipStyle(base.isEmpty() ? "Fusion" : base));
    }

    // ── Global key intercept + txtDepths focus events ─────────────────────────
    qApp->installEventFilter(this);

    buildUI();
    txtOutput->viewport()->installEventFilter(this);
    txtOutput->viewport()->setMouseTracking(true);

    // ── Load Abid's notation font (embedded resource) ─────────────────────────
    OutputConverter::loadAbidFont();
    buildStyles();
    if (m_mainWidget)
        m_mainWidget->setStyleSheet(buildStyleSheet());
    updateCommand();

    m_sliceTimer = new QTimer(this);
    m_sliceTimer->setSingleShot(true);
    connect(m_sliceTimer, &QTimer::timeout, this, [this]
            {
                int saves = (m_sliceCount % 2 == 0) ? 2 : 1;
                m_undoStack.append(m_slicePending.first());
                if (saves == 2 && m_slicePending.size() >= 2)
                    m_undoStack.append(m_slicePending.last());
                while (m_undoStack.size() > 64) m_undoStack.removeFirst();
                btnUndo->setEnabled(true);
                m_redoStack.clear();
                btnRedo->setEnabled(false);
                m_sliceCount = 0;
                m_slicePending.clear(); });

    loadSettings();
}

static QString invertScrambleStr(const QString &str)
{
    if (str.trimmed().isEmpty())
        return str;
    QStringList parts = str.trimmed().split('/');
    std::reverse(parts.begin(), parts.end());
    QStringList result;
    for (QString part : parts)
    {
        part = part.trimmed();
        QString inner = part;
        inner.remove('(');
        inner.remove(')');
        inner = inner.trimmed();
        if (inner.contains(','))
        {
            QStringList nums = inner.split(',');
            if (nums.size() == 2)
            {
                bool ok1, ok2;
                int a = nums[0].trimmed().toInt(&ok1);
                int b = nums[1].trimmed().toInt(&ok2);
                if (ok1 && ok2)
                {
                    result << QString("%1,%2").arg(-a).arg(-b);
                    continue;
                }
            }
        }
        else if (!inner.isEmpty())
        {
            bool ok1;
            int a = inner.toInt(&ok1);
            if (ok1)
            {
                result << QString::number(-a);
                continue;
            }
        }
        result << part;
    }
    return result.join("/");
}

void MainWindow::buildUI()
{
    // ── Zoom scaffold: QGraphicsView wraps everything so we can scale the whole UI ──
    m_mainWidget = new QWidget(); // this is the real UI root
    QWidget *realInner = m_mainWidget;
    m_zoomScene = new QGraphicsScene(this);
    m_zoomView = new QGraphicsView(m_zoomScene, this);
    m_zoomView->setFrameShape(QFrame::NoFrame);
    m_zoomView->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_zoomView->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_zoomView->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    m_zoomView->setRenderHint(QPainter::Antialiasing);
    m_zoomView->setStyleSheet("background: transparent; border: none;");
    m_zoomProxy = m_zoomScene->addWidget(realInner);
    setCentralWidget(m_zoomView);

    QWidget *outerWidget = realInner; // rest of buildUI builds into realInner
    QVBoxLayout *outerLayout = new QVBoxLayout(outerWidget);
    outerLayout->setContentsMargins(0, 0, 0, 0);
    outerLayout->setSpacing(0);

    // ── Top bar ───────────────────────────────────────────────────────────────
    QWidget *topBar = new QWidget();
    topBar->setObjectName("topBar");
    topBar->setFixedHeight(52);
    QHBoxLayout *topBarLayout = new QHBoxLayout(topBar);
    topBarLayout->setContentsMargins(16, 0, 16, 0);

    QLabel *logoLabel = new QLabel();
    auto updateLogo = [this, logoLabel]()
    {
        QString primary = Theme::textPrimary();
        QString muted = Theme::textMuted();
        logoLabel->setText(QString("<span style='font-size:17px;font-weight:bold;color:%1;letter-spacing:1px;'>CROISSANT</span>"
                                   "<br><span style='font-size:10px;color:%2;'>by Abid and Matt</span>")
                               .arg(primary, muted));
    };
    updateLogo();
    m_updateLogo = updateLogo;
    logoLabel->setObjectName("logoLabel");

    btnHamburger = new QPushButton("☰");
    btnHamburger->setObjectName("btnHamburger");
    btnHamburger->setFixedSize(30, 30);
    btnHamburger->setToolTip("Menu");
    connect(btnHamburger, &QPushButton::clicked, this, &MainWindow::openSidebar);

    topBarLayout->addWidget(btnHamburger);
    topBarLayout->addSpacing(10);
    topBarLayout->addWidget(logoLabel);
    topBarLayout->addStretch();
    outerLayout->addWidget(topBar);

    // ── Full-width input bar ──────────────────────────────────────────────────
    QWidget *inputBarOuter = new QWidget();
    inputBarOuter->setObjectName("inputBarOuter");
    m_inputBarOuter = inputBarOuter;

    QHBoxLayout *inputBarLay = new QHBoxLayout(inputBarOuter);
    inputBarLay->setContentsMargins(0, 0, 0, 0);
    inputBarLay->setSpacing(0);

    m_inputMode = new QPushButton("SCRAMBLE");
    m_inputMode->setObjectName("btnInputMode");

    m_inputModeArrow = new QPushButton("▾");
    m_inputModeArrow->setObjectName("btnInputModeArrow");

    m_mainInput = new QLineEdit();
    m_mainInput->setObjectName("txtMainInput");
    m_mainInput->setPlaceholderText("1,0 / 3,3 / 0,-3 / ...  (supports karn)");

    btnApply = new QPushButton("Apply");
    btnApply->setObjectName("btnApply");

    inputBarLay->addWidget(m_inputMode);
    inputBarLay->addWidget(m_inputModeArrow);
    inputBarLay->addWidget(m_mainInput, 1);
    inputBarLay->addWidget(btnApply);

    outerLayout->addWidget(inputBarOuter);

    QWidget *contentWidget = new QWidget();
    outerLayout->addWidget(contentWidget, 1);

    QHBoxLayout *root = new QHBoxLayout(contentWidget);
    root->setSpacing(12);
    root->setContentsMargins(12, 12, 12, 12);

    // ---- LEFT: cube widget + move buttons ----
    QWidget *leftContainer = new QWidget();
    leftContainer->setMinimumWidth(316);
    QVBoxLayout *leftCol = new QVBoxLayout(leftContainer);
    leftCol->setContentsMargins(0, 0, 0, 0);
    leftCol->setSpacing(4);
    cubeWidget = new Sq1Widget(this);
    connect(cubeWidget, &Sq1Widget::positionChanged, this, &MainWindow::updateCommand);
    connect(cubeWidget, &Sq1Widget::positionChanged, this, &MainWindow::updateConstraints);
    connect(cubeWidget, &Sq1Widget::userInteracted, this, &MainWindow::pushUndoState);
    connect(cubeWidget, &Sq1Widget::positionChanged, this, [this]()
            {
        bool cs = cubeWidget->inCubeshape();
        if (!cs && chkCubeshape->isChecked()) {
            chkCubeshape->blockSignals(true);
            chkCubeshape->setChecked(false);
            chkCubeshape->blockSignals(false);
            updateConstraints();
            updateCommand();
        }
        chkCubeshape->setEnabled(cs); });
    connect(cubeWidget, &Sq1Widget::equatorStateChanged, this, [this](int state)
            {
        bool shouldBeChecked = (state == 0);
        if (chkIgnoreEquator->isChecked() != shouldBeChecked) {
            if (!shouldBeChecked) {
                // leaving gray state — remember nothing, just uncheck
                m_preIgnoreMidState = 1;
            }
            chkIgnoreEquator->blockSignals(true);
            chkIgnoreEquator->setChecked(shouldBeChecked);
            chkIgnoreEquator->blockSignals(false);
            updateConstraints();
            updateCommand();
        } });
    {
        // cubeWithReset fills the full width of leftContainer; reset button overlaps absolutely
        QWidget *cubeWithReset = new QWidget();
        cubeWithReset->setObjectName("cubeWithReset");
        cubeWithReset->setAttribute(Qt::WA_StyledBackground, true);
        // Height: cube height + some padding for the reset button row at top
        cubeWithReset->setFixedHeight(cubeWidget->height() + 8);
        // Width is NOT fixed — it will stretch to fill leftContainer

        // cubeWrapper is centered inside cubeWithReset via absolute positioning after layout
        QWidget *cubeWrapper = new QWidget(cubeWithReset);
        cubeWrapper->setObjectName("cubeWrapper");
        cubeWrapper->setAttribute(Qt::WA_StyledBackground, true);
        cubeWrapper->setFixedSize(cubeWidget->width(), cubeWidget->height());
        cubeWidget->setParent(cubeWrapper);
        cubeWidget->move(0, 0);
        // cubeWrapper will be centered in cubeWithReset after show (via event filter / resize)

        btnReset = new QPushButton("Reset", cubeWithReset);
        btnReset->setObjectName("btnReset");
        btnReset->setToolTip("Reset  [Esc]");
        // Position reset button at top-right; it overlaps the cube area
        btnReset->move(cubeWithReset->width() - 58, 6);
        btnReset->raise();

        // Use a resize event filter to keep cubeWrapper centered and btnReset at top-right
        struct CubeResizeFilter : public QObject
        {
            QWidget *cubeWithReset;
            QWidget *cubeWrapper;
            QPushButton *btnReset;
            CubeResizeFilter(QWidget *p, QWidget *cwr, QWidget *cwrap, QPushButton *btn)
                : QObject(p), cubeWithReset(cwr), cubeWrapper(cwrap), btnReset(btn) {}
            bool eventFilter(QObject *watched, QEvent *e) override
            {
                if (e->type() == QEvent::Resize && watched == cubeWithReset)
                {
                    int cx = (cubeWithReset->width() - cubeWrapper->width()) / 2;
                    cubeWrapper->move(cx, 9);
                    btnReset->move(cubeWithReset->width() - 58, 6);
                }
                return false;
            }
        };
        cubeWithReset->installEventFilter(
            new CubeResizeFilter(cubeWithReset, cubeWithReset, cubeWrapper, btnReset));

        // Initial position (will be corrected on first resize)
        cubeWrapper->move((300 - cubeWidget->width()) / 2, 9);

        QHBoxLayout *centerRow = new QHBoxLayout();
        centerRow->setContentsMargins(0, 0, 0, 0);
        centerRow->addWidget(cubeWithReset); // stretches to fill
        leftCol->addLayout(centerRow);
    }

    // Grid: U' | Slice (rowspan 2) | U
    //        D |                   | D'
    m_moveButtonsWidget = new QWidget();
    QGridLayout *moveGrid = new QGridLayout(m_moveButtonsWidget);
    moveGrid->setSpacing(4);
    moveGrid->setContentsMargins(0, 0, 0, 0);

    QPushButton *btnUP = new QPushButton("U'");
    QPushButton *btnU = new QPushButton("U");
    QPushButton *btnD = new QPushButton("D");
    QPushButton *btnDP = new QPushButton("D'");
    QPushButton *btnSlice = new QPushButton();
    btnSlice->setText("Slice [I/K]");
    btnSlice->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

    moveGrid->addWidget(btnUP, 0, 0);
    moveGrid->addWidget(btnSlice, 0, 1, 2, 1);
    moveGrid->addWidget(btnU, 0, 2);
    moveGrid->addWidget(btnD, 1, 0);
    moveGrid->addWidget(btnDP, 1, 2);

    moveGrid->setColumnStretch(0, 1);
    moveGrid->setColumnStretch(1, 1);
    moveGrid->setColumnStretch(2, 1);
    moveGrid->setRowStretch(0, 1);
    moveGrid->setRowStretch(1, 1);

    leftCol->addWidget(m_moveButtonsWidget);

    // Row 3: Undo | Redo
    QHBoxLayout *undoResetRedoRow = new QHBoxLayout();
    undoResetRedoRow->setSpacing(4);
    btnUndo = new QPushButton("Undo (Ctrl+Z)");
    btnUndo->setObjectName("btnUndo");
    btnUndo->setEnabled(false);
    btnUndo->setToolTip("Undo  [Ctrl+Z]");
    btnRedo = new QPushButton("Redo (Ctrl+Y)");
    btnRedo->setObjectName("btnRedo");
    btnRedo->setEnabled(false);
    btnRedo->setToolTip("Redo  [Ctrl+Y]");
    undoResetRedoRow->addWidget(btnUndo, 1);
    undoResetRedoRow->addWidget(btnRedo, 1);
    leftCol->addLayout(undoResetRedoRow);

    btnSolve = new QPushButton("▶  Solve  [Ctrl+Enter]");
    btnSolve->setObjectName("btnSolve");
    btnSolve->setFixedHeight(48);
    leftCol->addWidget(btnSolve);

    leftCol->addStretch();

    leftContainer->setObjectName("leftPanel");
    leftScroll = new QScrollArea();
    leftScroll->setObjectName("leftScroll");
    m_leftPanel = leftScroll;
    leftScroll->setWidget(leftContainer);
    leftScroll->setWidgetResizable(false);
    leftScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    leftScroll->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    leftScroll->setFrameShape(QFrame::NoFrame);
    leftScroll->setMinimumWidth(320);
    leftScroll->setMaximumWidth(380);

    connect(btnU, &QPushButton::clicked, cubeWidget, [this]
            { pushUndoState(); cubeWidget->setFocus(); QKeyEvent e(QEvent::KeyPress,Qt::Key_J,Qt::NoModifier); QApplication::sendEvent(cubeWidget,&e); });
    connect(btnUP, &QPushButton::clicked, cubeWidget, [this]
            { pushUndoState(); cubeWidget->setFocus(); QKeyEvent e(QEvent::KeyPress,Qt::Key_F,Qt::NoModifier); QApplication::sendEvent(cubeWidget,&e); });
    connect(btnSlice, &QPushButton::clicked, cubeWidget, [this]
            {
                cubeWidget->setFocus();
                m_sliceCount++;
                m_slicePending.append({cubeWidget->getPositionString()});
                QKeyEvent e(QEvent::KeyPress, Qt::Key_I, Qt::NoModifier);
                QApplication::sendEvent(cubeWidget, &e);
                m_sliceTimer->start(600); });
    connect(btnD, &QPushButton::clicked, cubeWidget, [this]
            { pushUndoState(); cubeWidget->setFocus(); QKeyEvent e(QEvent::KeyPress,Qt::Key_S,Qt::NoModifier); QApplication::sendEvent(cubeWidget,&e); });
    connect(btnDP, &QPushButton::clicked, cubeWidget, [this]
            { pushUndoState(); cubeWidget->setFocus(); QKeyEvent e(QEvent::KeyPress,Qt::Key_L,Qt::NoModifier); QApplication::sendEvent(cubeWidget,&e); });
    connect(btnReset, &QPushButton::clicked, this, [this]
            {
                QString before = cubeWidget->getPositionString();
                cubeWidget->reset();
                QString after = cubeWidget->getPositionString();
                if (before != after) {
                    m_undoStack.append({before});
                    if (m_undoStack.size() > 64) m_undoStack.removeFirst();
                    btnUndo->setEnabled(true);
                    m_redoStack.clear();
                    btnRedo->setEnabled(false);
                }
                onReset(); });
    connect(btnUndo, &QPushButton::clicked, this, [this]
            {
                if (m_undoStack.isEmpty()) return;
                m_redoStack.append({cubeWidget->getPositionString()});
                if (m_redoStack.size() > 64) m_redoStack.removeFirst();
                btnRedo->setEnabled(true);
                CubeSnapshot snap = m_undoStack.takeLast();
                cubeWidget->setPositionFromString(snap.posStr);
                updateCommand();
                btnUndo->setEnabled(!m_undoStack.isEmpty()); });
    connect(btnRedo, &QPushButton::clicked, this, [this]
            {
                if (m_redoStack.isEmpty()) return;
                m_undoStack.append({cubeWidget->getPositionString()});
                if (m_undoStack.size() > 64) m_undoStack.removeFirst();
                btnUndo->setEnabled(true);
                CubeSnapshot snap = m_redoStack.takeLast();
                cubeWidget->setPositionFromString(snap.posStr);
                updateCommand();
                btnRedo->setEnabled(!m_redoStack.isEmpty()); });

    root->addWidget(leftScroll);

    // ---- RIGHT: options + output ----
    QVBoxLayout *rightCol = new QVBoxLayout();
    rightCol->setSpacing(6);

    QGroupBox *grpOptions = new QGroupBox("Options");
    grpOptions->setMinimumHeight(200);

    QWidget *optionsInner = new QWidget();
    optionsInner->setObjectName("optionsInner");
    QVBoxLayout *grid = new QVBoxLayout(optionsInner);
    grid->setContentsMargins(0, 0, 0, 0);
    grid->setSpacing(0);

    auto makeRow = [](const QString &objName = "optionRow") -> QWidget *
    {
        QWidget *row = new QWidget();
        row->setObjectName(objName);
        row->setAttribute(Qt::WA_StyledBackground, true);
        QHBoxLayout *l = new QHBoxLayout(row);
        l->setContentsMargins(0, 0, 0, 0);
        l->setSpacing(0);
        return row;
    };
    auto rowLeft = [](QWidget *row) -> QHBoxLayout *
    {
        QWidget *left = new QWidget();
        left->setObjectName("optionRowLeft");
        left->setAttribute(Qt::WA_StyledBackground, true);
        QHBoxLayout *ll = new QHBoxLayout(left);
        ll->setContentsMargins(0, 0, 0, 0);
        ll->setSpacing(0);
        static_cast<QHBoxLayout *>(row->layout())->addWidget(left, 1);
        return ll;
    };
    auto rowRight = [](QWidget *row) -> QHBoxLayout *
    {
        QWidget *right = new QWidget();
        right->setObjectName("optionRowRight");
        right->setAttribute(Qt::WA_StyledBackground, true);
        QHBoxLayout *rl = new QHBoxLayout(right);
        rl->setContentsMargins(0, 0, 0, 0);
        rl->setSpacing(0);
        static_cast<QHBoxLayout *>(row->layout())->addWidget(right);
        return rl;
    };

    // ── Widgets ──────────────────────────────────────────────────────────────
    // ── Metric radio: Slice (default) | Move | Angle ─────────────────────────
    auto makeRadioRow = [this](const QString &labelText, const QStringList &opts,
                               int defaultId, QButtonGroup *&groupOut,
                               const QString &rowName, const QString &pillName) -> QWidget *
    {
        QWidget *row = new QWidget();
        row->setObjectName(rowName);
        row->setAttribute(Qt::WA_StyledBackground, true);
        QHBoxLayout *rLay = new QHBoxLayout(row);
        rLay->setContentsMargins(0, 0, 0, 0);
        rLay->setSpacing(0);

        QLabel *lbl = new QLabel(labelText);
        lbl->setObjectName(rowName + "_label");
        lbl->setStyleSheet(QString("color: %1; font-size: 12px;").arg(Theme::textSecondary()));
        rLay->addWidget(lbl);
        rLay->addStretch();

        QWidget *pill = new QWidget();
        pill->setObjectName(pillName);
        pill->setAttribute(Qt::WA_StyledBackground, true);
        QHBoxLayout *pLay = new QHBoxLayout(pill);
        pLay->setContentsMargins(2, 1, 2, 1);
        pLay->setSpacing(0);

        groupOut = new QButtonGroup(this);
        groupOut->setExclusive(true);
        for (int i = 0; i < opts.size(); i++)
        {
            QPushButton *btn = new QPushButton(opts[i]);
            btn->setCheckable(true);
            btn->setChecked(i == defaultId);
            btn->setCursor(Qt::PointingHandCursor);
            if (i == 0)
                btn->setObjectName(pillName + "_first");
            else if (i == opts.size() - 1)
                btn->setObjectName(pillName + "_last");
            else
                btn->setObjectName(pillName + "_mid");
            groupOut->addButton(btn, i);
            pLay->addWidget(btn);
        }
        rLay->addWidget(pill);
        return row;
    };

    m_metricRadioRow = makeRadioRow("Metric", {"Slice", "Move", "Angle"}, 0, m_metricGroup, "metricRadioRow", "metricPill");
    QWidget *metricRadioRow = m_metricRadioRow;
    metricRadioRow->setToolTip("Choose how move length of an alg is counted:\n"
                               "Slice – only slices count\n"
                               "Move – layer turns count too\n"
                               "Angle – layer turns are weighted by angle amount");
    // (pill objectName set inside makeRadioRow)

    chkAllOptimal = new TightCheckBox("All optimal");
    chkAllOptimal->setObjectName("chkAllOptimal");
    chkAllOptimal->setToolTip("Find all the optimal solutions, not just the first one.");

    spnSuboptimal = new QSpinBox();
    spnSuboptimal->setObjectName("spnSuboptimal");
    spnSuboptimal->setRange(0, 9);
    spnSuboptimal->setValue(0);
    spnSuboptimal->setToolTip("Extra moves beyond optimal to ALSO find (0 = optimal only).");

    lblSuboptLabel = new QLabel("+suboptimal:");
    lblSuboptLabel->setObjectName("lblSuboptLabel");
    lblSuboptLabel->setStyleSheet(QString("color: %1; font-size: 12px;").arg(Theme::textSecondary()));
    lblSuboptLabel->setToolTip(spnSuboptimal->toolTip());

    chkDepths = new TightCheckBox("Specific depths:");
    chkDepths->setObjectName("chkDepths");
    chkDepths->setToolTip("Search only the listed move depths, instead of starting from 0 and going up.\n"
                          "Comma-separated, e.g.\"8,9\". \n"
                          "Write in the input box to toggle it on.");
    chkDepths->setAttribute(Qt::WA_TransparentForMouseEvents, false);
    chkDepths->setFocusPolicy(Qt::NoFocus);

    txtDepths = new QLineEdit();
    txtDepths->setObjectName("txtDepths");
    txtDepths->setPlaceholderText("e.g. 8,9");
    txtDepths->setValidator(new QRegularExpressionValidator(
        QRegularExpression("[0-9,]*"), txtDepths));
    txtDepths->setToolTip("Comma-separated list of depths to search, e.g. \"8,9\"");

    chkGenerator = new TightCheckBox("Generator alg");
    chkGenerator->setObjectName("chkGenerator");
    chkGenerator->setToolTip("If selected, generated algs will set up to the case from a solved cube,\n"
                             "else the algs will solve the case.");

    m_twoGenRadioRow = makeRadioRow("2 Gen", {"2 Gen", "Pseudo 2 Gen", "None"}, 2, m_twoGenGroup, "twoGenRadioRow", "twoGenPill");
    QWidget *twoGenRadioRow = m_twoGenRadioRow;
    twoGenRadioRow->setToolTip("2 Gen \342\200\223 restrict to top-layer turns and slices only\n"
                               "Pseudo 2 Gen \342\200\223 restrict bottom-layer turns to \302\2611 only\n"
                               "None \342\200\223 no 2-gen restriction");

    chkCubeshape = new TightCheckBox("Stay in cubeshape");
    chkCubeshape->setObjectName("chkCubeshape");
    chkCubeshape->setToolTip("Only generate algs that keep the puzzle in cubeshape throughout.");

    chkIgnoreEquator = new TightCheckBox("Ignore equator");
    chkIgnoreEquator->setObjectName("chkIgnoreEquator");
    chkIgnoreEquator->setToolTip("Ignore equator states. Equivalent to clicking on the bar until it is gray.");

    chkKarnotation = new TightCheckBox("Karn output");
    chkKarnotation->setObjectName("chkKarnotation");
    chkKarnotation->setToolTip("Display solutions in karnotation instead of WCA notation.");

    m_angleRadioRow = makeRadioRow("Lock layer angle on preabf",
                                   {"Both", "Top", "Bottom", "None"}, 3, m_angleGroup, "angleRadioRow", "anglePill");
    QWidget *angleRadioRow = m_angleRadioRow;
    angleRadioRow->setToolTip("Lock the pre-ABF angle move to ±1 or 0.\n"
                              "Both – restricts top and bottom\n"
                              "Top – restricts top layer only\n"
                              "Bottom – restricts bottom layer only\n"
                              "None – no restriction");

    m_normalizeAbfRow = makeRadioRow("Normalize ABF",
                                     {"Both", "PreABF", "PostABF", "None"}, 3, m_normalizeAbfGroup, "normalizeAbfRow", "normalizeAbfPill");
    m_normalizeAbfRow->setEnabled(true);
    QWidget *normalizeAbfRow = m_normalizeAbfRow;
    // TODO: update this tooltip text with a precise description of what normalizing does
    normalizeAbfRow->setToolTip("Control which ABF moves are normalized in the output. (e.g. 3-1 → 0-1, 43 → 10)\n"
                                "PreABF – normalize the move before the first slice\n"
                                "PostABF – normalize the move after the last slice\n"
                                "Both – normalize both pre- and post-ABF\n"
                                "None – no normalization");

    chkMaxX = new TightCheckBox("Max top turn:");
    chkMaxX->setObjectName("chkMaxX");
    chkMaxX->setToolTip("Limit the maximum top-layer turn in either direction (0–6).\n"
                        "e.g. if you put \"4\", that means algs can do -4 to 4 on top.");
    spnMaxX = new QSpinBox();
    spnMaxX->setObjectName("spnMaxX");
    spnMaxX->setRange(0, 6);
    spnMaxX->setValue(3);
    spnMaxX->setToolTip("Maximum top-layer turn in either direction (0–6).\n"
                        "e.g. if you put \"4\", that means algs can do -4 to 4 on top.");

    chkMaxY = new TightCheckBox("Max bottom turn:");
    chkMaxY->setObjectName("chkMaxY");
    chkMaxY->setToolTip("Limit the maximum bottom-layer turn in either direction (0–6).\n"
                        "e.g. if you put \"3\", that means algs can do -3 to 3 on bottom.");
    spnMaxY = new QSpinBox();
    spnMaxY->setObjectName("spnMaxY");
    spnMaxY->setRange(0, 6);
    spnMaxY->setValue(3);
    spnMaxY->setToolTip("Maximum bottom-layer turn in either direction (0–6).\n"
                        "e.g. if you put \"3\", that means algs can do -3 to 3 on bottom.");

    chkMaxTotal = new TightCheckBox("Max total turn:");
    chkMaxTotal->setObjectName("chkMaxTotal");
    chkMaxTotal->setToolTip("Limit the maximum combined |top|+|bottom| turn per move pair (1–12).\n"
                            "e.g. if you put \"6\", (3,-3) is allowed in algs (3+3<=6),\n"
                            "but (-5,-2) is not (5+2>6).");
    spnMaxTotal = new QSpinBox();
    spnMaxTotal->setObjectName("spnMaxTotal");
    spnMaxTotal->setRange(1, 12);
    spnMaxTotal->setValue(6);
    spnMaxTotal->setToolTip("Maximum combined |top|+|bottom| turn per move pair (1–12).\n"
                            "e.g. if you put \"6\", (3,-3) is allowed in algs (3+3<=6),\n"
                            "but (-5,-2) is not (5+2>6).");

    chkKarnotation->setChecked(true);

    // ── Div-style rows ───────────────────────────────────────────────────────
    // Row: Metric radio (full width)
    {
        QWidget *row = makeRow("optionRow_metric");
        rowLeft(row)->addWidget(metricRadioRow, 1);
        grid->addWidget(row);
    }
    // Row: All optimal + suboptimal
    {
        QWidget *row = makeRow("optionRow_allopt");
        m_allOptRow = row;
        rowLeft(row)->addWidget(chkAllOptimal);
        QHBoxLayout *rr = rowRight(row);
        rr->addWidget(lblSuboptLabel);
        rr->addSpacing(4);
        rr->addWidget(spnSuboptimal);
        grid->addWidget(row);
    }
    // Row: Specific depths
    {
        QWidget *row = makeRow("optionRow_depths");
        rowLeft(row)->addWidget(chkDepths);
        rowRight(row)->addWidget(txtDepths);
        grid->addWidget(row);
    }
    // Row: Generator alg (full width)
    {
        QWidget *row = makeRow("optionRow_generator");
        rowLeft(row)->addWidget(chkGenerator);
        grid->addWidget(row);
    }
    // Row: 2 Gen radio (full width)
    {
        QWidget *row = makeRow("optionRow_2gen");
        rowLeft(row)->addWidget(twoGenRadioRow, 1);
        grid->addWidget(row);
    }
    // Row: Stay in cubeshape (full width)
    {
        QWidget *row = makeRow("optionRow_cubeshape");
        rowLeft(row)->addWidget(chkCubeshape);
        grid->addWidget(row);
    }
    // Row: Ignore middle layer (full width)
    {
        QWidget *row = makeRow("optionRow_ignoremid");
        rowLeft(row)->addWidget(chkIgnoreEquator);
        grid->addWidget(row);
    }
    // Row: Angle radio (full width)
    {
        QWidget *row = makeRow("optionRow_angle");
        rowLeft(row)->addWidget(angleRadioRow, 1);
        grid->addWidget(row);
    }
    // Row: Max top turn
    {
        QWidget *row = makeRow("optionRow_maxx");
        rowLeft(row)->addWidget(chkMaxX);
        rowRight(row)->addWidget(spnMaxX);
        grid->addWidget(row);
    }
    // Row: Max bottom turn
    {
        QWidget *row = makeRow("optionRow_maxy");
        rowLeft(row)->addWidget(chkMaxY);
        rowRight(row)->addWidget(spnMaxY);
        grid->addWidget(row);
    }
    // Row: Max total turn
    {
        QWidget *row = makeRow("optionRow_maxtotal");
        rowLeft(row)->addWidget(chkMaxTotal);
        rowRight(row)->addWidget(spnMaxTotal);
        grid->addWidget(row);
    }
    grid->addStretch();

    QScrollArea *optionsScroll = new QScrollArea();
    optionsScroll->setWidget(optionsInner);
    optionsScroll->setWidgetResizable(true);
    optionsScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    optionsScroll->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    optionsScroll->setFrameShape(QFrame::NoFrame);
    optionsScroll->setMinimumHeight(120);
    grpOptions->setLayout(new QVBoxLayout());
    grpOptions->layout()->setContentsMargins(4, 16, 4, 4);
    grpOptions->layout()->addWidget(optionsScroll);

    // ── Connections ──────────────────────────────────────────────────────────
    auto upd = [this]
    { updateConstraints(); updateCommand(); };

    connect(m_metricGroup, QOverload<int>::of(&QButtonGroup::idClicked), this, [upd](int)
            { upd(); });
    connect(chkAllOptimal, &QCheckBox::toggled, this, upd);
    connect(spnSuboptimal, QOverload<int>::of(&QSpinBox::valueChanged), this, upd);
    connect(chkDepths, &QCheckBox::clicked, this, [this](bool)
            {
        // Clicking directly is not allowed — state is driven by txtDepths content
        QString t = txtDepths->text().trimmed();
        bool valid = !t.isEmpty() && QRegularExpression("^[0-9]+(,[0-9]+)*$").match(t).hasMatch();
        chkDepths->blockSignals(true);
        chkDepths->setChecked(valid);
        chkDepths->blockSignals(false); });
    connect(txtDepths, &QLineEdit::textChanged, this, [this, upd](const QString &text)
            {
        QString t = text.trimmed();
        // Valid = non-empty and only digits and commas, at least one digit
        bool valid = !t.isEmpty() && QRegularExpression("^[0-9]+(,[0-9]+)*$").match(t).hasMatch();
        chkDepths->blockSignals(true);
        chkDepths->setChecked(valid);
        chkDepths->blockSignals(false);
        upd(); });
    connect(chkGenerator, &QCheckBox::toggled, this, upd);
    connect(m_twoGenGroup, QOverload<int>::of(&QButtonGroup::idClicked), this, [upd](int)
            { upd(); });
    connect(chkCubeshape, &QCheckBox::toggled, this, upd);
    connect(chkIgnoreEquator, &QCheckBox::toggled, this, [this, upd](bool checked)
            {
        if (checked) {
            m_preIgnoreMidState = cubeWidget->getEquatorState();
            cubeWidget->setEquatorState(0); // ignore
        } else {
            cubeWidget->setEquatorState(m_preIgnoreMidState);
        }
        upd(); });
    connect(chkKarnotation, &QCheckBox::toggled, this, [this, upd](bool /*checked*/)
            {
        upd();
        if (!m_rawLines.isEmpty()) {
            if (m_tableVisible)
                rebuildTable();
            else if (m_ratingsValid && m_cubeshapeWasActive)
                onRankErgoToggled(true);
            else
                rebuildTerminalView();
        } });
    connect(m_angleGroup, QOverload<int>::of(&QButtonGroup::idClicked), this, [upd](int)
            { upd(); });
    connect(m_normalizeAbfGroup, QOverload<int>::of(&QButtonGroup::idClicked), this, [this, upd](int id)
            {
        m_normalizeAbfMode = id;
        if (!m_rawLines.isEmpty()) {
            if (m_tableVisible) rebuildTable();
            else if (m_ratingsValid && m_cubeshapeWasActive) onRankErgoToggled(true);
            else rebuildTerminalView();
        } });
    connect(chkMaxX, &QCheckBox::toggled, this, upd);
    connect(spnMaxX, QOverload<int>::of(&QSpinBox::valueChanged), this, upd);
    connect(chkMaxY, &QCheckBox::toggled, this, upd);
    connect(spnMaxY, QOverload<int>::of(&QSpinBox::valueChanged), this, upd);
    connect(chkMaxTotal, &QCheckBox::toggled, this, upd);
    connect(spnMaxTotal, QOverload<int>::of(&QSpinBox::valueChanged), this, upd);

    // ── Pack options/command/solve/progress into one hideable wrapper ─────────
    m_topSection = new QWidget();
    QVBoxLayout *topLay = new QVBoxLayout(m_topSection);
    topLay->setContentsMargins(0, 0, 0, 0);
    topLay->setSpacing(6);
    topLay->addWidget(grpOptions);

    txtCommand = new QLineEdit();
    txtCommand->setReadOnly(false);
    txtCommand->setObjectName("txtCommand");
    txtCommand->setVisible(false);

    btnCopy = new QPushButton("⎘");
    btnCopy->setObjectName("btnCopy");
    btnCopy->setFixedWidth(32);
    btnCopy->setFixedHeight(24);
    btnCopy->setToolTip("Copy the alg");
    btnCopy->setVisible(false);

    // Enter/Shift+Enter are handled in eventFilter; returnPressed is not used.

    m_mainInput->installEventFilter(this);

    // Ctrl+Enter triggers Solve / Stop from anywhere in the window
    QShortcut *solveShortcut = new QShortcut(QKeySequence("Ctrl+Return"), this);
    connect(solveShortcut, &QShortcut::activated, this, &MainWindow::onSolveButtonClicked);

    connect(btnApply, &QPushButton::clicked, this, [this]
            {
                const QString text = m_mainInput->text().trimmed();
                if (text.isEmpty() && m_inputModeIndex == 2) return;

                if (m_inputModeIndex == 2) {
                    pushUndoState();
                    bool ok = cubeWidget->setPositionFromString(text);
                    if (!ok) {
                        m_mainInput->setProperty("hasError", true);
                        style()->polish(m_mainInput);
                        m_undoStack.removeLast();
                        btnUndo->setEnabled(!m_undoStack.isEmpty());
                    } else {
                        m_mainInput->setProperty("hasError", false);
                        style()->polish(m_mainInput);
                        updateCommand();
                    }
                    return;
                }

                pushUndoState();

                if (m_applyFromSolved) {
                    cubeWidget->reset();
                }

                QString raw = text.trimmed().isEmpty() ? "0,0" : text;
                raw = raw.trimmed();
                if (m_inputModeIndex == 1) {
                    raw = invertScrambleStr(raw);
                }

                // Capture leading/trailing slash from raw BEFORE the addComma pass
                // replaces all '/' with spaces. We restore them after unkarnifyHelp
                // so the move parser sees e.g. "/3,0/0,3/" correctly.
                bool leadingSlash  = !raw.isEmpty() && (raw[0] == '/' || raw[0] == '\\');
                bool trailingSlash = raw.size() > 1 && (raw[raw.size()-1] == '/' || raw[raw.size()-1] == '\\');

                // addCommas rules
                {
                    raw.replace('/', ' ').replace('\\', ' ');
                    raw = raw.simplified();
                    QStringList tokens = raw.split(' ', Qt::SkipEmptyParts);
                    for (QString &tok : tokens) {
                        QString t = tok;
                        t.remove('('); t.remove(')');
                        if (t.isEmpty() || t.contains(',')) continue;
                        bool allNumeric = true;
                        for (QChar ch : t)
                            if (ch != '-' && !ch.isDigit()) { allNumeric = false; break; }
                        if (!allNumeric) continue;
                        if (t.size() == 1)
                            tok = t + ",0";
                        else if (t.size() == 2)
                            tok = (t[0] == '-') ? t + ",0"
                                                : QString(t[0]) + "," + t[1];
                        else if (t.size() == 3)
                            tok = (t[0] == '-') ? t.left(2) + "," + t[2]
                                                : QString(t[0]) + "," + t.mid(1);
                        else if (t.size() == 4)
                            tok = t.left(2) + "," + t.mid(2);
                    }
                    raw = tokens.join(' ');
                }
                // Step 3: unkarnify — KARN_TO_WCA dict replacement + shorthand resolution.
                // replaceShorthands(unkarnifyHelp(s)) mirrors the JS step 8 pipeline:
                //   unkarnifyHelp applies KARN_TO_WCA on the space-separated token stream
                //   and normalises to slash-separated output;
                //   replaceShorthands then resolves alignment-dependent shorthand tokens
                //   (bjj, fv10, kk0-1, …) which are not in KARN_TO_WCA directly.
                {
                    std::string s = raw.toStdString();
                    if (leadingSlash && s.front() != '/') s = "/" + s;
                    if (trailingSlash && s.back() != '/') s = s + "/";
                    s = replaceShorthands(unkarnifyHelp(s));
                    raw = QString::fromStdString(s);
                }

                // Restore leading/trailing slashes that were stripped by the addComma pass.
                // Karn tokens already produce their surrounding slashes via unkarnifyHelp,
                // so only add if not already present.
                if (leadingSlash  && !raw.startsWith('/')) raw.prepend('/');
                if (trailingSlash && !raw.endsWith('/'))   raw.append('/');

                struct Move { bool isSlice; int x, y; };
                QVector<Move> moves;
                bool ok = true;

                QStringList segments = raw.split('/');
                for (int si = 0; si < segments.size() && ok; si++) {
                    QString seg = segments[si].trimmed();
                    seg.remove('('); seg.remove(')');

                    if (si > 0) moves.append({true, 0, 0});

                    if (seg.isEmpty()) continue;

                    if (seg.contains(',')) {
                        QStringList parts = seg.split(',');
                        if (parts.size() != 2) { ok = false; break; }
                        bool ok1, ok2;
                        int x = parts[0].trimmed().toInt(&ok1);
                        int y = parts[1].trimmed().toInt(&ok2);
                        if (!ok1 || !ok2) { ok = false; break; }
                        moves.append({false, x, y});
                    } else {
                        bool ok1;
                        int x = seg.toInt(&ok1);
                        if (!ok1) { ok = false; break; }
                        moves.append({false, x, 0});
                    }
                }

                if (!ok) {
                    m_mainInput->setProperty("hasError", true);
                    style()->polish(m_mainInput);
                    m_undoStack.removeLast();
                    btnUndo->setEnabled(!m_undoStack.isEmpty());
                    return;
                }

                QVector<Sq1Widget::MoveStep> widgetMoves;
                for (const Move& mv : moves)
                    widgetMoves.append({mv.isSlice, mv.x, mv.y});

                bool applied = cubeWidget->applyMoves(widgetMoves);
                if (!applied) {
                    const QString errMsg =
                        "Position after applying this alg is not sliceable — "
                        "a corner is split across the cut line. "
                        "The alg may be incomplete or incorrect.";
                    QToolTip::showText(
                        m_mainInput->mapToGlobal(QPoint(0, m_mainInput->height())),
                        errMsg, m_mainInput, {}, 4000);
                    m_mainInput->setProperty("hasError", true);
                    style()->polish(m_mainInput);
                    m_undoStack.removeLast();
                    btnUndo->setEnabled(!m_undoStack.isEmpty());
                } else {
                    m_mainInput->setProperty("hasError", false);
                    QToolTip::hideText();
                    style()->polish(m_mainInput);
                    updateCommand();
                } });

    connect(m_inputMode, &QPushButton::clicked, this, [this]
            {
                m_inputModeIndex = (m_inputModeIndex + 1) % 3;
                if (m_inputModeIndex == 0) { m_inputMode->setText("SCRAMBLE"); m_mainInput->setPlaceholderText("1,0 / 3,3 / 0,-3 / ...  (supports karn)"); }
                else if (m_inputModeIndex == 1) { m_inputMode->setText("ALG");  m_mainInput->setPlaceholderText("1,0 / 3,3 / 0,-3 / ...  (supports karn)"); }
                else                           { m_inputMode->setText("POSITION"); m_mainInput->setPlaceholderText("ABCDEFGH12345678-"); }
                m_mainInput->clear(); });

    connect(m_inputModeArrow, &QPushButton::clicked, this, [this]
            {
                QMenu* menu = new QMenu(this);
                menu->setStyleSheet(QString(
                    "QMenu { background: %1; border: 1px solid %2; border-radius: 6px; padding: 4px; color: %3; font-size: 12px; }"
                    "QMenu::item { padding: 6px 20px; border-radius: 4px; }"
                    "QMenu::item:selected { background: %4; }"
                    "QMenu::item:checked { color: %5; font-weight: bold; }"
                    ).arg(Theme::menuBg(), Theme::menuBorder(), Theme::fadingTooltipText(),
                          Theme::menuItemSelected(), Theme::menuItemChecked()));
                QAction* aScram = menu->addAction("Scramble");
                QAction* aAlg   = menu->addAction("Alg");
                QAction* aPos   = menu->addAction("Position");
                aScram->setCheckable(true); aScram->setChecked(m_inputModeIndex == 0);
                aAlg->setCheckable(true);   aAlg->setChecked(m_inputModeIndex == 1);
                aPos->setCheckable(true);   aPos->setChecked(m_inputModeIndex == 2);
                connect(aScram, &QAction::triggered, this, [this]{ m_inputModeIndex = 0; m_inputMode->setText("SCRAMBLE"); m_mainInput->setPlaceholderText("1,0 / 3,3 / 0,-3 / ...  (supports karn)"); m_mainInput->clear(); });
                connect(aAlg,   &QAction::triggered, this, [this]{ m_inputModeIndex = 1; m_inputMode->setText("ALG");      m_mainInput->setPlaceholderText("1,0 / 3,3 / 0,-3 / ...  (supports karn)"); m_mainInput->clear(); });
                connect(aPos,   &QAction::triggered, this, [this]{ m_inputModeIndex = 2; m_inputMode->setText("POSITION"); m_mainInput->setPlaceholderText("ABCDEFGH12345678-");                        m_mainInput->clear(); });
                menu->exec(m_inputModeArrow->mapToGlobal(QPoint(0, m_inputModeArrow->height()))); });

    connect(m_mainInput, &QLineEdit::textChanged, this, [this](const QString &text)
            {
                m_mainInput->setProperty("hasError", false);
                style()->polish(m_mainInput);
                if (m_inputModeIndex == 2) {
                    if (text.trimmed().isEmpty()) {
                        m_mainInput->setProperty("hasError", false);
                        style()->polish(m_mainInput);
                        return;
                    }
                    return;
                } else {
                    return; {
                        if (false) {

                            QString raw = text.trimmed();
                            if (m_inputModeIndex == 1) {
                                raw = invertScrambleStr(raw);
                            }

                            std::string karnStr = raw.toStdString();
                            bool hasAlpha = false;
                            for (char c : karnStr) if (std::isalpha((unsigned char)c)) { hasAlpha = true; break; }
                            if (hasAlpha) {
                                std::string converted = unkarnify(karnStr);
                                raw = QString::fromStdString(converted);
                            }

                            raw.replace('\\', '/');

                            struct Move { bool isSlice; int x, y; };
                            QVector<Move> moves;
                            bool ok = true;

                            QStringList segments = raw.split('/');
                            for (int si = 0; si < segments.size() && ok; si++) {
                                QString seg = segments[si].trimmed();
                                seg.remove('('); seg.remove(')');

                                if (si > 0) moves.append({true, 0, 0});

                                if (seg.isEmpty()) continue;

                                if (seg.contains(',')) {
                                    QStringList parts = seg.split(',');
                                    if (parts.size() != 2) { ok = false; break; }
                                    bool ok1, ok2;
                                    int x = parts[0].trimmed().toInt(&ok1);
                                    int y = parts[1].trimmed().toInt(&ok2);
                                    if (!ok1 || !ok2) { ok = false; break; }
                                    moves.append({false, x, y});
                                } else {
                                    bool ok1;
                                    int x = seg.toInt(&ok1);
                                    if (!ok1) { ok = false; break; }
                                    moves.append({false, x, 0});
                                }
                            }

                            if (!ok) {
                                m_mainInput->setProperty("hasError", true);
                                style()->polish(m_mainInput);
                                cubeWidget->reset();
                                updateCommand();
                                return;
                            }

                            int pos[24] = {};
                            int mid = 0;
                            {
                                std::string s = cubeWidget->getPositionString().toStdString();
                                int j = 0, nextPC = -3, nextPE = 18;
                                for (int i = 0; i < 16 && j < 24; i++) {
                                    int k = (unsigned char)s[i];
                                    if (k >= 'a' && k <= 'z') k += ('A'-'a');
                                    if      (k>='A'&&k<='H') k-='A';
                                    else if (k>='1'&&k<='8') k-=('1'-8);
                                    else if (k=='U'||k=='V') { k=nextPC; nextPC-=3; }
                                    else if (k=='W')         { k=nextPC; nextPC-=3; }
                                    else if (k=='X'||k=='Y') { k=nextPE; nextPE+=3; }
                                    else if (k=='Z')         { k=nextPE; nextPE+=3; }
                                    pos[j++]=k;
                                    if (k>=0&&k<8) pos[j++]=k;
                                }
                                mid = (s.size()>=17) ? (s[16]=='/'?1:0) : (!s.empty()&&s.back()=='/'?1:0);
                            }

                            auto doTop = [&](int m){ m=((m%12)+12)%12; for(int mv=0;mv<m;mv++){ int c=pos[11]; for(int i=11;i>0;i--) pos[i]=pos[i-1]; pos[0]=c; } };
                            auto doBot = [&](int m){ m=((m%12)+12)%12; for(int mv=0;mv<m;mv++){ int c=pos[23]; for(int i=23;i>12;i--) pos[i]=pos[i-1]; pos[12]=c; } };
                            auto canSlice = [&](){ return pos[0]!=pos[11]&&pos[5]!=pos[6]&&pos[12]!=pos[23]&&pos[17]!=pos[18]; };
                            auto doSlice = [&](){ if(!canSlice()) return; for(int i=6;i<12;i++) std::swap(pos[i],pos[i+6]); mid=1-mid; };

                            for (const Move& mv : moves) {
                                if (mv.isSlice) doSlice();
                                else { doTop(mv.x); doBot(mv.y); }
                            }

                            const QString pieceChars = "ABCDEFGH12345678";
                            QString posStr;
                            for (int i = 0; i < 24; i++) {
                                posStr += pieceChars[pos[i]];
                                if (pos[i] < 8) i++;
                            }
                            posStr += (mid == 0 ? '-' : '/');

                            bool applied = cubeWidget->setPositionFromString(posStr);
                            if (!applied) {
                                m_mainInput->setProperty("hasError", true);
                                style()->polish(m_mainInput);
                                cubeWidget->reset();
                            }
                            updateCommand();
                        } }
                } });

    lblCommandError = new QLabel("");
    lblCommandError->setObjectName("lblCommandError");
    lblCommandError->setWordWrap(true);
    lblCommandError->setVisible(false);
    topLay->addWidget(lblCommandError);

    progressBar = new QProgressBar();
    progressBar->setRange(0, 0);
    progressBar->setVisible(false);
    progressBar->setFixedHeight(6);
    topLay->addWidget(progressBar);

    rightCol->addWidget(m_topSection);

    // Output stack wrapper with floating buttons
    QWidget *outputWrapper = new QWidget();
    outputWrapper->setMinimumHeight(120);
    QVBoxLayout *outputWrapperLay = new QVBoxLayout(outputWrapper);
    outputWrapperLay->setContentsMargins(0, 0, 0, 0);
    outputWrapperLay->setSpacing(0);

    txtOutput = new QTextEdit(outputWrapper);
    txtOutput->setReadOnly(true);
    txtOutput->setObjectName("txtOutput");
    txtOutput->setMinimumHeight(120);
    // Do not set Qt::NoContextMenu here — it redirects ContextMenu events to the
    // parent before our eventFilter can catch them, breaking right-click entirely.
    txtOutput->document()->setDefaultStyleSheet("div, span { background: transparent !important; }");

    btnExpand = new QPushButton("⤢", outputWrapper);
    btnExpand->setObjectName("btnExpand");
    btnExpand->setFixedSize(22, 22);
    btnExpand->setToolTip("Expand terminal");
    btnExpand->installEventFilter(this);
    {
        auto *e = new QGraphicsOpacityEffect(btnExpand);
        e->setOpacity(1.0);
        btnExpand->setGraphicsEffect(e);
    }

    btnCopyTerminal = new QPushButton("⧉", outputWrapper);
    btnCopyTerminal->setObjectName("btnCopyTerminal");
    btnCopyTerminal->setFixedSize(22, 22);
    btnCopyTerminal->setToolTip("Copy all algs in terminal");
    btnCopyTerminal->installEventFilter(this);
    {
        auto *e = new QGraphicsOpacityEffect(btnCopyTerminal);
        e->setOpacity(1.0);
        btnCopyTerminal->setGraphicsEffect(e);
    }

    btnFavorites = new QPushButton("♥", outputWrapper);
    btnFavorites->setObjectName("btnFavorites");
    btnFavorites->setFixedSize(22, 22);
    btnFavorites->setToolTip("Open the favorites bin");
    btnFavorites->installEventFilter(this);
    {
        auto *e = new QGraphicsOpacityEffect(btnFavorites);
        e->setOpacity(1.0);
        btnFavorites->setGraphicsEffect(e);
    }

    btnTableMode = new QPushButton("⊞", outputWrapper);
    btnTableMode->setObjectName("btnTableMode");
    btnTableMode->setFixedSize(22, 22);
    btnTableMode->setToolTip("Switch to table view");
    btnTableMode->installEventFilter(this);
    {
        auto *e = new QGraphicsOpacityEffect(btnTableMode);
        e->setOpacity(1.0);
        btnTableMode->setGraphicsEffect(e);
    }

    btnScrollToBottom = new QPushButton("↓", outputWrapper);
    btnScrollToBottom->setObjectName("btnScrollToBottom");
    btnScrollToBottom->setFixedSize(32, 32);
    btnScrollToBottom->setToolTip("Scroll to bottom and resume auto-scroll");
    btnScrollToBottom->setVisible(false);
    btnScrollToBottom->setCursor(Qt::PointingHandCursor);

    // Favorites / table / expand are always available (even on startup);
    // copy-terminal only appears once there are algs in the terminal.
    btnExpand->setVisible(true);
    btnCopyTerminal->setVisible(false);
    btnFavorites->setVisible(true);
    btnTableMode->setVisible(true);

    // Idle-fade timer — fires 1.5 s after the last mouse move inside the output area.
    // Scrolling (wheel events) does not reset this; only actual mouse movement does.
    m_outputIdleTimer = new QTimer(this);
    m_outputIdleTimer->setSingleShot(true);
    connect(m_outputIdleTimer, &QTimer::timeout, this, [this]()
            { setOutputBtnsOpacity(0.15, 400); });

    // Mouse tracking so we receive QEvent::MouseMove without a button held down
    outputWrapper->setMouseTracking(true);

    outputWrapperLay->addWidget(txtOutput);
    auto pauseAutoScroll = [this]()
    {
        if (!m_autoScrollPaused && worker && worker->isRunning())
        {
            m_autoScrollPaused = true;
            btnScrollToBottom->setGraphicsEffect(nullptr);
            btnScrollToBottom->setText("⌄");
            btnScrollToBottom->setStyleSheet(""); // revert to QSS default
            int w = m_outputWrapper->width();
            int h = m_outputWrapper->height();
            btnScrollToBottom->move(w - 6 - 28 - 16, h - 6 - 32 - 6);
            btnScrollToBottom->raise();
            btnScrollToBottom->setVisible(true);
        }
    };
    connect(txtOutput->verticalScrollBar(), &QScrollBar::sliderPressed, this, pauseAutoScroll);
    connect(txtOutput->verticalScrollBar(), &QScrollBar::actionTriggered, this, [pauseAutoScroll](int action)
            {
        // SliderSingleStepAdd/Sub = arrow keys/buttons, SliderPageStepAdd/Sub = track click
        if (action == QAbstractSlider::SliderPageStepAdd ||
            action == QAbstractSlider::SliderPageStepSub ||
            action == QAbstractSlider::SliderMove)
            pauseAutoScroll(); });

    m_tableContainer = new QWidget();
    m_tableContainer->setVisible(false);
    m_tableContainer->setMinimumHeight(120);
    QVBoxLayout *tableLay = new QVBoxLayout(m_tableContainer);
    tableLay->setContentsMargins(0, 0, 0, 0);
    tableLay->setSpacing(4);

    m_solutionTable = new QTableWidget();
    m_solutionTable->setObjectName("m_solutionTable");
    m_solutionTable->setColumnCount(4);
    m_solutionTable->setHorizontalHeaderLabels({"#", "Solution", "Moves", "Slices"});
    m_solutionTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_solutionTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_solutionTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_solutionTable->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    m_solutionTable->verticalHeader()->setVisible(false);
    m_solutionTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_solutionTable->setSelectionMode(QAbstractItemView::NoSelection);
    m_solutionTable->setFocusPolicy(Qt::NoFocus);
    m_solutionTable->setShowGrid(false);
    m_solutionTable->setAlternatingRowColors(false);
    m_solutionTable->setTextElideMode(Qt::ElideNone);
    m_solutionTable->setContextMenuPolicy(Qt::NoContextMenu);
    m_solutionTable->viewport()->setCursor(Qt::ArrowCursor);
    m_solutionTable->viewport()->setMouseTracking(true);
    tableLay->addWidget(m_solutionTable, 1);

    outputWrapperLay->addWidget(m_tableContainer);
    m_outputWrapper = outputWrapper;
    outputWrapper->installEventFilter(this);
    rightCol->addWidget(outputWrapper, 1);

    rightCol->addWidget(chkKarnotation);
    rightCol->addWidget(normalizeAbfRow);

    lblStatus = new QLabel("");
    lblStatus->setObjectName("lblStatus");
    lblStatus->setVisible(false);

    root->addLayout(rightCol, 1);

    // ── Button connections ────────────────────────────────────────────────────
    connect(btnSolve, &QPushButton::clicked, this, &MainWindow::onSolveButtonClicked);
    connect(btnCopy, &QPushButton::clicked, this, &MainWindow::onCopy);
    connect(btnExpand, &QPushButton::clicked, this, &MainWindow::toggleExpand);
    connect(btnFavorites, &QPushButton::clicked, this, &MainWindow::showFavoritesModal);
    connect(btnCopyTerminal, &QPushButton::clicked, this, [this]
            {
                // Always copy clean (non-abid) text regardless of display mode.
                const QStringList &lines = chkKarnotation->isChecked() ? m_karnLines : m_rawLines;
                QApplication::clipboard()->setText(lines.join('\n'));
                appendStatusLine("Terminal copied to clipboard!"); });
    connect(btnScrollToBottom, &QPushButton::clicked, this, [this]
            {
                // If solve is done and we were paused, transition to table
                if (m_solveFinishedWhilePaused) {
                    m_autoScrollPaused = false;
                    m_solveFinishedWhilePaused = false;
                    btnScrollToBottom->setVisible(false);
                    m_tableVisible = true;
                    txtOutput->setVisible(false);
                    m_tableContainer->setVisible(true);
                    btnTableMode->setText("▤");
                    btnTableMode->setToolTip("Switch to terminal view");
                    rebuildTable();
                    return;
                }
                // Otherwise just resume auto-scroll
                m_autoScrollPaused = false;
                m_solveFinishedWhilePaused = false;
                btnScrollToBottom->setVisible(false);
                btnScrollToBottom->setText("⌄");
                btnScrollToBottom->setGraphicsEffect(nullptr);
                btnScrollToBottom->setText("⌄");
                btnScrollToBottom->setStyleSheet(
                    "QPushButton#btnScrollToBottom {"
                    "  background: #2a2a4a; border: 1px solid #4a4a7a; border-radius: 16px;"
                    "  color: #aaaaff; font-size: 16px; font-weight: bold;"
                    "  padding: 0px; margin: 0px; text-align: center; line-height: 32px; }"
                    "QPushButton#btnScrollToBottom:hover { background: #3a3a6a; }");
                btnScrollToBottom->setToolTip("Scroll to bottom and resume auto-scroll");
                txtOutput->verticalScrollBar()->setValue(
                    txtOutput->verticalScrollBar()->maximum()); });
    connect(btnTableMode, &QPushButton::clicked, this, [this]
            {
                m_tableVisible = !m_tableVisible;
                txtOutput->setVisible(!m_tableVisible);
                m_tableContainer->setVisible(m_tableVisible);
                btnTableMode->setText(m_tableVisible ? "▤" : "⊞");
                btnTableMode->setToolTip(m_tableVisible ? "Switch to terminal view" : "Switch to table view");
                if (m_tableVisible) rebuildTable();
                else if (m_ratingsValid && m_cubeshapeWasActive) onRankErgoToggled(true);
                else rebuildTerminalView(); });
    connect(txtCommand, &QLineEdit::textEdited, this, [this](const QString &text)
            {
                auto showCmdError = [this](const QString& msg) {
                    lblCommandError->setText(msg);
                    lblCommandError->setVisible(true);
                    // Error color is theme-dependent; pick the right value directly.
                    QString errCol = Theme::textError();
                    txtCommand->setStyleSheet(QString(
                        "QLineEdit#txtCommand { font-family: monospace; color: %1;"
                        " font-size: 12px; border-color: %1; border-right: none;"
                        " border-radius: 0; border-top-left-radius: 4px; border-bottom-left-radius: 4px; }")
                        .arg(errCol));
                };
                auto clearCmdError = [this]() {
                    lblCommandError->setVisible(false);
                    txtCommand->setStyleSheet(""); // revert to QSS
                };

                QStringList parts = text.trimmed().split(' ', Qt::SkipEmptyParts);
                if (parts.isEmpty()) { clearCmdError(); return; }

                QString pos = parts.last();

                if (pos.startsWith('-') || pos.contains(',')) {
                    showCmdError("No position string at end of command — last token looks like a flag.");
                    return;
                }
                if (pos.length() < 15 || pos.length() > 17) {
                    if (pos.length() > 0)
                        showCmdError(QString("Position string should be 15–17 characters, got %1.").arg(pos.length()));
                    else
                        clearCmdError();
                    return;
                }

                static const QString validChars = "ABCDEFGHabcdefgh12345678UVWXYZuvwxyz-/";
                bool validCharsOk = true;
                for (int ci = 0; ci < pos.length(); ci++) {
                    if (!validChars.contains(pos[ci])) {
                        validCharsOk = false;
                        break;
                    }
                }
                if (!validCharsOk) {
                    showCmdError("Invalid character in position string.");
                    return;
                }

                bool applied = cubeWidget->setPositionFromString(pos);
                if (!applied) {
                    showCmdError("Invalid position string — duplicate or unrecognised pieces.");
                    return;
                }
                clearCmdError(); });

    QTimer::singleShot(0, this, [this]
                       {
                           int w = m_outputWrapper->width();
                           int margin = 6; int bw = 22;
                           btnExpand->move(w - margin - bw, margin);
                           btnTableMode->move(w - margin - bw*2 - 4, margin);
                           btnFavorites->move(w - margin - bw*3 - 8, margin);
                           btnCopyTerminal->move(w - margin - bw*4 - 12, margin);
                           btnExpand->raise(); btnTableMode->raise(); btnFavorites->raise(); btnCopyTerminal->raise(); });

    const auto allBtns = findChildren<QPushButton *>();
    for (auto *b : allBtns)
        b->setCursor(Qt::PointingHandCursor);
    const auto allChks = findChildren<QCheckBox *>();
    for (auto *c : allChks)
        c->setCursor(Qt::PointingHandCursor);
    m_solutionTable->setCursor(Qt::ArrowCursor);

    updateConstraints();
    rebuildTerminalView();
}
// -------------------------------------------------------
// toggleExpand — expand / shrink the output terminal
// -------------------------------------------------------
void MainWindow::toggleExpand()
{
    m_expanded = !m_expanded;
    m_topSection->setVisible(!m_expanded);
    m_leftPanel->setVisible(!m_expanded);
    if (m_expanded)
    {
        btnExpand->setText("⤡");
        btnExpand->setToolTip("Shrink terminal");
    }
    else
    {
        btnExpand->setText("⤢");
        btnExpand->setToolTip("Expand terminal");
    }
    // Rerender output/table with expanded styles
    if (m_tableVisible)
        rebuildTable();
    else if (m_ratingsValid && m_cubeshapeWasActive)
        onRankErgoToggled(true);
    else
        rebuildTerminalView();
}

void MainWindow::rebuildTerminalView()
{
    txtOutput->clear();
    // ── Debug lines ───────────────────────────────────────────
    if (m_debugOutput && !m_debugBuffer.isEmpty())
    {
        QTextCursor cur(txtOutput->document());
        QTextCharFormat fmt;
        fmt.setForeground(QColor(Theme::textMuted()));
        fmt.setFontItalic(false);
        fmt.setFontPointSize(m_expanded ? 11 : 9);
        QTextBlockFormat blkFmt;
        blkFmt.setLineHeight(120, QTextBlockFormat::ProportionalHeight);
        for (const QString &dbg : std::as_const(m_debugBuffer))
        {
            cur.setBlockFormat(blkFmt);
            cur.insertText(dbg, fmt);
            cur.insertBlock();
        }
    }
    // Choose which line list to display
    const QStringList &lines = chkKarnotation->isChecked() ? m_karnLines : m_rawLines;
    if (lines.isEmpty())
    {
        QTextCursor cur(txtOutput->document());
        QTextCharFormat fmt;
        fmt.setForeground(QColor("#2a2a3a"));
        fmt.setFontItalic(false);
        fmt.setFontPointSize(10);
        fmt.setFontFamily("monospace");
        cur.insertText("solution will be displayed here...", fmt);
        return;
    }

    // Helper: insert a solution line with optional Abid font on the alg portion.
    auto insertSolLine = [this](QTextCursor &cur, const QString &line, const QTextCharFormat &fmt)
    {
        if (!m_abidNotation || OutputConverter::s_abidFontFamily.isEmpty())
        {
            cur.insertText(line, fmt);
            return;
        }
        int lb = line.lastIndexOf('[');
        QString algPart = lb > 0 ? line.left(lb).trimmed() : line;
        QString bracketPart = lb > 0 ? "  " + line.mid(lb).trimmed() : QString();
        QTextCharFormat abidFmt = fmt;
        abidFmt.setFontFamily(OutputConverter::s_abidFontFamily);
        abidFmt.setFontPointSize(fmt.fontPointSize() + 2);
        cur.insertText(OutputConverter::abidifyDisplay(algPart), abidFmt);
        if (!bracketPart.isEmpty())
            cur.insertText(bracketPart, fmt);
    };

    QTextCursor cur(txtOutput->document());
    cur.movePosition(QTextCursor::End);
    int solIdx = 0;
    QSet<QString> seenAlgs;
    for (int lineIdx = 0; lineIdx < lines.size(); lineIdx++)
    {
        const QString &rawLine = lines[lineIdx];
        bool isSol = rawLine.contains('[') && rawLine.contains(']');
        QString line = isSol ? applyNormalizeAbf(rawLine) : rawLine;
        if (isSol)
        {
            int lb = line.lastIndexOf('[');
            QString key = lb > 0 ? line.left(lb).trimmed() : line.trimmed();
            if (seenAlgs.contains(key))
            {
                continue;
            }
            seenAlgs.insert(key);
        }
        if (!cur.atStart())
            cur.insertBlock();
        QTextCharFormat fmt;
        if (isSol)
        {
            bool isAlt = (solIdx % 2 == 1);
            QString col = isAlt ? Theme::solutionAltLight() : Theme::textSolution();
            fmt.setForeground(QColor(col));
            fmt.setFontWeight(m_expanded ? QFont::Bold : QFont::Normal);
            fmt.setFontPointSize(m_expanded ? 13 : 10);
            QTextBlockFormat blkFmt;
            blkFmt.setLineHeight(m_expanded ? 180 : 120, QTextBlockFormat::ProportionalHeight);
            cur.setBlockFormat(blkFmt);
            insertSolLine(cur, line, fmt);
            // Store the displayed line so context menu / favorites gets what is shown
            cur.block().setUserData(new AlgBlockData(line));
            // Per-alg debug annotation right after the alg
            if (m_debugOutput && solIdx > 0 && solIdx - 1 < m_algAnnLines.size() && !m_algAnnLines[solIdx - 1].isEmpty())
            {
                cur.insertBlock();
                QTextCharFormat afmt;
                afmt.setForeground(QColor(Theme::textMuted()));
                afmt.setFontItalic(false);
                afmt.setFontPointSize(m_expanded ? 11 : 9);
                QTextBlockFormat ablkFmt;
                ablkFmt.setLineHeight(120, QTextBlockFormat::ProportionalHeight);
                cur.setBlockFormat(ablkFmt);
                cur.insertText(m_algAnnLines[solIdx - 1], afmt);
            }
            solIdx++;
        }
        else
        {
            fmt.setForeground(QColor(Theme::textMuted()));
            fmt.setFontWeight(QFont::Normal);
            fmt.setFontPointSize(m_expanded ? 11 : 10);
            QTextBlockFormat blkFmt;
            blkFmt.setLineHeight(m_expanded ? 150 : 120, QTextBlockFormat::ProportionalHeight);
            cur.setBlockFormat(blkFmt);
            cur.insertText(line, fmt);
        }
    }
    txtOutput->verticalScrollBar()->setValue(0);
}

// -------------------------------------------------------
// twoGenCompatibility — pure position check, no Qt dependencies.
// Returns 2 if the position supports 2-gen (full block on bottom),
//         1 if it supports pseudo-2-gen only (CEC block on bottom),
//         0 if neither applies.
// Delegates to the shared twoGenPreADF() so the block tables live in exactly
// one place (see sq1opt-runner.h / FullPosition::findPreADF).
// -------------------------------------------------------
static int twoGenCompatibility(const Sq1Widget::RawState &s)
{
    if (!twoGenPreADF(s.pos, 2, /*firstMatchOnly=*/true).empty())
        return 2;
    if (!twoGenPreADF(s.pos, 1, /*firstMatchOnly=*/true).empty())
        return 1;
    return 0;
}

// -------------------------------------------------------
// updateConstraints
// -------------------------------------------------------
void MainWindow::updateConstraints()
{
    // Don't re-enable any widgets while the solver is running — setSolverRunning
    // owns the enabled state during that time.
    const bool solverRunning = worker && worker->isRunning();

    const int tgId = m_twoGenGroup ? m_twoGenGroup->checkedId() : 2;
    const bool isAllOpt = chkAllOptimal->isChecked();
    const bool isDepths = chkDepths->isChecked();

    // Auto-deselect "Specific depths" when the input field is cleared.
    if (isDepths && txtDepths->text().trimmed().isEmpty())
    {
        chkDepths->blockSignals(true);
        chkDepths->setChecked(false);
        chkDepths->blockSignals(false);
    }
    const bool isDepthsNow = chkDepths->isChecked(); // re-read after possible uncheck

    if (!solverRunning)
        chkCubeshape->setEnabled(cubeWidget->inCubeshape());

    spnSuboptimal->setVisible(isAllOpt && !isDepthsNow);
    if (lblSuboptLabel)
        lblSuboptLabel->setVisible(isAllOpt && !isDepthsNow);

    // txtDepths is always enabled so the user can click into it and activate the option.
    // Style it to look inactive when the checkbox is off.
    txtDepths->setProperty("inactive", !isDepthsNow);
    txtDepths->style()->polish(txtDepths);

    if (!solverRunning)
    {
        spnMaxX->setEnabled(chkMaxX->isChecked());
        spnMaxY->setEnabled(chkMaxY->isChecked());
        spnMaxTotal->setEnabled(chkMaxTotal->isChecked());
    }

    // 2-gen / pseudo-2-gen compatibility: if the selected mode requires a solved
    // CECE/CEC block on the bottom but none exists, disable Solve with a tooltip.
    if (!solverRunning)
    {
        const Sq1Widget::RawState rs = cubeWidget->getRawState();
        const int compat = twoGenCompatibility(rs);
        const bool is2gen = (tgId == 0 || tgId == 1);
        bool blocked = (tgId == 0 && compat < 2) || (tgId == 1 && compat < 1);
        const char *msg = nullptr;
        if (blocked)
        {
            msg = (tgId == 0)
                      ? "Position is not compatible with 2-gen:\n"
                        "no solved corner-edge-corner-edge block found on the bottom layer."
                      : "Position is not compatible with pseudo-2-gen:\n"
                        "no solved corner-edge-corner block found on the bottom layer.";
        }
        // When keeping cube shape with a 2-gen mode, the corner permutation must
        // also be solvable with 2-gen moves (in addition to the block check above),
        // checked once per valid preADF candidate. Handles concrete and partial.
        else if (is2gen && chkCubeshape->isChecked() && cubeWidget->inCubeshape() && !cornersAre2GenSolvable(rs.pos, (tgId == 0 ? 2 : 1)))
        {
            blocked = true;
            msg = "Position is not compatible with 2-gen while keeping cube shape:\n"
                  "the corner permutation cannot be solved with 2-gen moves.";
        }
        btnSolve->setEnabled(!blocked);
        btnSolve->setToolTip(blocked ? QString::fromUtf8(msg) : QString());
    }
}

// -------------------------------------------------------
// buildStyles
// -------------------------------------------------------
void MainWindow::buildStyles()
{
    setStyleSheet(buildStyleSheet());
}

QString MainWindow::buildStyleSheet()
{
    return ::buildStyleSheet();
}

// -------------------------------------------------------
// buildArgList
// -------------------------------------------------------
QStringList MainWindow::buildArgList()
{
    QStringList args;

    // Metric radio: 0=Slice (default/UI), 1=Move (solver default), 2=Angle
    // Slice is the UI default but the solver defaults to TURN_METRIC, so always emit the flag.
    {
        int id = m_metricGroup ? m_metricGroup->checkedId() : 0;
        if (id == 0)
            args << "-es"; // slice
        else if (id == 2)
            args << "-ea"; // angle
        // id == 1 (move/turn): no flag — solver default
    }

    if (chkAllOptimal->isChecked())
    {
        const bool useNumber = !chkDepths->isChecked() && spnSuboptimal->value() > 0;
        args << (useNumber ? QString("-a%1").arg(spnSuboptimal->value()) : QString("-a"));
    }

    if (chkDepths->isChecked())
    {
        QString dv = txtDepths->text().trimmed().remove(' ');
        if (!dv.isEmpty())
            args << QString("-d%1").arg(dv);
    }

    if (chkGenerator->isChecked())
        args << "-g";
    {
        int id = m_twoGenGroup ? m_twoGenGroup->checkedId() : 2;
        if (id == 0)
            args << "-2";
        else if (id == 1)
            args << "-p";
        // id == 2 (None): no flag
    }
    if (chkCubeshape->isChecked())
        args << "-c";
    if (chkIgnoreEquator->isChecked())
        args << "-m";
    // Angle lock radio: 0=Both, 1=Top, 2=Bottom, 3=None (default — no flag)
    {
        int id = m_angleGroup ? m_angleGroup->checkedId() : 3;
        if (id == 0)
            args << "-nb";
        else if (id == 1)
            args << "-nu";
        else if (id == 2)
            args << "-nd";
        // id == 3 (None): no flag
    }

    if (chkMaxX->isChecked())
        args << QString("-X%1").arg(spnMaxX->value());
    if (chkMaxY->isChecked())
        args << QString("-Y%1").arg(spnMaxY->value());
    if (chkMaxTotal->isChecked())
        args << QString("-Z%1").arg(spnMaxTotal->value());

    if (m_ignoreTrans)
        args << "-x";

    return args;
}

void MainWindow::updateCommand()
{
    QString pos = cubeWidget->getPositionString();
    QStringList args = buildArgList();
    txtCommand->setText("sq1opt " + args.join(" ") + " " + pos);
    lblCommandError->setVisible(false);
    txtCommand->setStyleSheet(""); // revert any error-state override to QSS
}

// -------------------------------------------------------
// onSolveButtonClicked — single entry-point for the Solve/Stop button
// -------------------------------------------------------
void MainWindow::onSolveButtonClicked()
{
    if (worker && worker->isRunning())
        stopSolver();
    else if (btnSolve->isEnabled()) // honour the disabled state (e.g. via Ctrl+Enter)
        onSolve();
}

// -------------------------------------------------------
// debugLine — emit a [DEBUG] status line if debug output is enabled.
// Writes directly to txtOutput and stores in m_debugBuffer so it
// survives terminal rebuilds without ever entering m_rawLines.
// -------------------------------------------------------
void MainWindow::debugLine(const QString &msg)
{
    if (!m_debugOutput)
        return;
    QString text = "[DEBUG] " + msg;
    // Write directly to txtOutput with non-italic muted styling
    {
        int savedScroll = txtOutput->verticalScrollBar()->value();
        QTextCursor cur = txtOutput->textCursor();
        cur.movePosition(QTextCursor::End);
        if (!txtOutput->document()->isEmpty())
            cur.insertBlock();
        QTextCharFormat fmt;
        fmt.setForeground(QColor(Theme::textMuted()));
        fmt.setFontItalic(false);
        fmt.setFontPointSize(m_expanded ? 11 : 9);
        QTextBlockFormat blkFmt;
        blkFmt.setLineHeight(120, QTextBlockFormat::ProportionalHeight);
        cur.setBlockFormat(blkFmt);
        cur.insertText(text, fmt);
        QTextCursor endCur = txtOutput->textCursor();
        endCur.movePosition(QTextCursor::End);
        txtOutput->setTextCursor(endCur);
        if (m_autoScrollPaused)
            txtOutput->verticalScrollBar()->setValue(savedScroll);
        else
            txtOutput->verticalScrollBar()->setValue(
                txtOutput->verticalScrollBar()->maximum());
    }
    m_debugBuffer.append(text);
}

// -------------------------------------------------------
// onSolve
// -------------------------------------------------------
void MainWindow::onSolve()
{
    if (worker && worker->isRunning())
        return;

    m_stopped = false;
    m_autoScrollPaused = false;
    m_solveFinishedWhilePaused = false;
    {
        QStringList args = buildArgList();
        m_lastRunKey = cubeWidget->getPositionString();
        if (!args.isEmpty())
            m_lastRunKey += " " + args.join(" ");
    }
    btnScrollToBottom->setVisible(false);
    txtOutput->clear();
    m_rawLines.clear();
    m_karnLines.clear();
    m_solutionLines.clear();
    m_karnSolutionLines.clear();
    m_solutionLinesForRating.clear();
    m_sliceIndicators.clear();
    m_rawFinalScores.clear();
    m_cachedRatedOrder.clear();
    m_ratingsValid = false;
    m_seenSolutions.clear();
    m_seenNormalizedAlgs.clear();
    m_debugBuffer.clear();
    m_algAnnLines.clear();
    // Always go back to terminal view while solving
    m_tableVisible = false;
    txtOutput->setVisible(true);
    m_tableContainer->setVisible(false);
    btnTableMode->setText("⊞");
    btnTableMode->setToolTip("Switch to table view");
    m_ratingsValid = false;
    m_cachedRatedOrder.clear();

    txtOutput->clear();
    // Hide copy-terminal until a solution actually arrives (it's re-shown in
    // onSolverLine on the first solution). Keep the always-on buttons visible.
    btnCopyTerminal->setVisible(false);
    btnFavorites->setVisible(true);
    btnTableMode->setVisible(true);
    btnExpand->setVisible(true);
    appendStatusLine("Solving…");

    // Swap Solve → Stop appearance (muted dark red, not alarming).
    btnSolve->setText("■  Stop  [Ctrl+Enter]");
    btnSolve->setStyleSheet(QString(
                                "QPushButton#btnSolve {"
                                "  background: %1; border: 1px solid %2; padding-top: 0px; padding-bottom: 0px;"
                                "  color: %3; font-size: 12px; font-weight: bold; }"
                                "QPushButton#btnSolve:hover { background: %4; }")
                                .arg(Theme::buttonStopBg(), Theme::buttonStopBorder(), Theme::buttonStopText(), Theme::buttonStopHover()));

    progressBar->setVisible(true);

    worker = new SolverWorker();
    {
        Sq1Widget::RawState rs = cubeWidget->getRawState();
        sq1optSetPosition(rs.pos, rs.middle);
        if (m_debugOutput)
        {
            QString posArr;
            for (int i = 0; i < 24; i++)
                posArr += QString::number(rs.pos[i]) + (i < 23 ? "," : "");
            debugLine(QString("injected pos=[%1] middle=%2").arg(posArr).arg(rs.middle));
        }
    }
    worker->positionStr = cubeWidget->getPositionString(); // still used for display/copy
    m_posHex = worker->positionStr;
    worker->flags = buildArgList();
    debugLine("positionStr=" + worker->positionStr + "  args=[" + worker->flags.join(" ") + "]");
    m_cubeshapeWasActive = chkCubeshape->isChecked();
    connect(worker, &SolverWorker::lineReady, this, &MainWindow::onSolverLine, Qt::QueuedConnection);
    connect(worker, &SolverWorker::finished, this, &MainWindow::onSolverDone, Qt::QueuedConnection);
    connect(worker, &SolverWorker::finished, worker, &QObject::deleteLater);
    m_solveStartMs = QDateTime::currentMSecsSinceEpoch();
    m_firstSolutionMs = 0;
    m_hadFirstSolution = false;
    // Disable all interactive controls while solver runs; only Stop is usable.
    setSolverRunning(true);
    worker->start();
}

// -------------------------------------------------------
// stopSolver — kill the running process and flag m_stopped
// -------------------------------------------------------
void MainWindow::stopSolver()
{
    m_stopped = true;
    if (worker && worker->isRunning())
        worker->requestStop();
}

// -------------------------------------------------------
// setOutputBtnsOpacity — animate all three floating toolbar
// buttons (copy / switch-view / expand) to the given opacity.
//
// Full opacity (target >= 1.0): the QGraphicsOpacityEffect is DISABLED rather
// than set to 1.0.  An enabled effect at opacity=1.0 still routes through an
// offscreen pixmap that breaks compositing over QTextEdit, making buttons
// invisible.  Disabling the effect lets buttons paint through the normal path.
//
// Fading (target < 1.0): the effect is enabled (starting at 0.99 if it was
// previously disabled) and animated to the target.  Starting at 0.99 avoids
// a single invisible frame that would occur if we enabled at exactly 1.0.
// -------------------------------------------------------
void MainWindow::setOutputBtnsOpacity(qreal target, int durationMs)
{
    m_outputBtnsFullOpacity = (target >= 1.0 - 0.01);
    QPushButton *btns[4] = {btnCopyTerminal, btnFavorites, btnTableMode, btnExpand};

    for (auto *btn : btns)
    {
        if (!btn)
            continue;
        auto *eff = qobject_cast<QGraphicsOpacityEffect *>(btn->graphicsEffect());
        if (!eff)
            continue;

        // Stop any running animation on this effect
        for (auto *a : btn->findChildren<QPropertyAnimation *>())
            if (a->targetObject() == eff)
                a->stop();

        if (target >= 1.0 - 0.01)
        {
            // Full opacity: disable the effect so the button uses the normal paint
            // path.  An enabled effect at 1.0 produces an invisible compositing glitch.
            eff->setEnabled(false);
            continue;
        }

        // Fading: enable the effect if it is currently disabled.  Start at 0.99
        // (not 1.0) so the very first rendered frame is never at the broken opacity.
        if (!eff->isEnabled())
        {
            eff->setOpacity(0.99);
            eff->setEnabled(true);
        }

        qreal startOpacity = eff->opacity();
        if (qAbs(startOpacity - target) < 0.01)
            continue;

        if (durationMs <= 0)
        {
            eff->setOpacity(target);
            continue;
        }

        auto *anim = new QPropertyAnimation(eff, "opacity", btn);
        anim->setDuration(durationMs);
        anim->setStartValue(startOpacity);
        anim->setEndValue(target);
        anim->setEasingCurve(QEasingCurve::InCubic);
        anim->start(QAbstractAnimation::DeleteWhenStopped);
    }
}

// -------------------------------------------------------
// onOutputMouseActive — called on every MouseMove inside the
// output wrapper (wheel events excluded).  Resets the idle
// timer and restores full opacity if the buttons were faded.
// -------------------------------------------------------
void MainWindow::onOutputMouseActive()
{
    // Don't restart the idle timer while a button is hovered — MouseMove on a
    // button child still reaches this path, but the Enter handler owns the timer
    // in that state.
    bool anyBtnHovered = btnCopyTerminal->underMouse() || btnFavorites->underMouse() || btnTableMode->underMouse() || btnExpand->underMouse();
    if (!anyBtnHovered && m_outputIdleTimer)
        m_outputIdleTimer->start(1500);
    if (!m_outputBtnsFullOpacity)
        setOutputBtnsOpacity(1.0, 0);
}

// -------------------------------------------------------
// setSolverRunning — disable/enable all interactive controls
// while the solver is active. btnSolve itself is handled
// separately (it becomes the Stop button).
// -------------------------------------------------------
void MainWindow::setSolverRunning(bool running)
{
    // Options panel widgets
    auto setRowEnabled = [&](QWidget *row, bool enabled)
    {
        if (!row)
            return;
        row->setEnabled(enabled);
        for (QWidget *child : row->findChildren<QWidget *>())
        {
            child->style()->unpolish(child);
            child->style()->polish(child);
            child->update();
        }
        // Drive label color directly — QSS :disabled via parent propagation
        // is unreliable across Qt versions without an explicit polish pass.
        const QString labelColor = enabled
                                       ? QString("color: %1; font-size: 12px;").arg(Theme::textSecondary())
                                       : QString("color: %1; font-size: 12px;").arg(Theme::textDisabled());
        if (QLabel *lbl = row->findChild<QLabel *>(row->objectName() + "_label"))
            lbl->setStyleSheet(labelColor);
    };
    setRowEnabled(m_metricRadioRow, !running);
    setRowEnabled(m_twoGenRadioRow, !running);
    setRowEnabled(m_angleRadioRow, !running);
    setRowEnabled(m_normalizeAbfRow, !running);
    // Qt doesn't reliably re-polish children disabled via parent propagation,
    // so drive the label color directly — inline style always wins over QSS.
    if (lblSuboptLabel)
        lblSuboptLabel->setStyleSheet(
            running ? QString("color: %1; font-size: 12px;").arg(Theme::textDisabled())
                    : QString("color: %1; font-size: 12px;").arg(Theme::textSecondary()));
    // allOptRow: disable both the row and widgets directly — direct setEnabled
    // calls survive Qt's parent-propagation and guard against updateConstraints interference.
    if (m_allOptRow)
        m_allOptRow->setEnabled(!running);
    chkAllOptimal->setEnabled(!running);
    spnSuboptimal->setEnabled(!running);
    chkDepths->setEnabled(!running);
    txtDepths->setEnabled(!running);
    chkGenerator->setEnabled(!running);
    chkCubeshape->setEnabled(!running);
    chkIgnoreEquator->setEnabled(!running);
    chkMaxX->setEnabled(!running);
    spnMaxX->setEnabled(!running);
    chkMaxY->setEnabled(!running);
    spnMaxY->setEnabled(!running);
    chkMaxTotal->setEnabled(!running);
    spnMaxTotal->setEnabled(!running);

    // Output options (below terminal)
    chkKarnotation->setEnabled(!running);

    // Input bar
    btnApply->setEnabled(!running);
    m_inputMode->setEnabled(!running);
    m_inputModeArrow->setEnabled(!running);
    m_mainInput->setEnabled(!running);

    // Cube interaction buttons (left panel)
    cubeWidget->setEnabled(!running);
    m_moveButtonsWidget->setEnabled(!running);
    btnUndo->setEnabled(!running && !m_undoStack.isEmpty());
    btnRedo->setEnabled(!running && !m_redoStack.isEmpty());
    btnReset->setEnabled(!running);

    // Settings modal checkboxes — disable if the modal is currently open.
    // We DON'T disable btnHamburger itself; the menu can open but settings
    // checkboxes inside it will be grayed out when running.
    if (chkSmartKarn)
        chkSmartKarn->setEnabled(!running);
    if (chkIgnoreTransSetting)
        chkIgnoreTransSetting->setEnabled(!running);
    // Abid notation toggle: find them by their checked state
    // in the settings card if it's open. Since the modal is rebuilt each open,
    // we reach them through the overlay child hierarchy.
    if (auto *central = this->centralWidget())
    {
        // Find the settings card if it's currently shown
        for (QWidget *child : central->findChildren<QWidget *>("settingsCard"))
        {
            for (QCheckBox *cb : child->findChildren<QCheckBox *>())
            {
                cb->setEnabled(!running);
            }
        }
    }
}

// -------------------------------------------------------
// injectSliceIndicatorDisplay (file-scope helper)
// Injects sliceStr at the first slice boundary of a display line (alg part only).
// The bracket "[x|y]" is never touched.
//
// Rules:
//   Raw WCA format  "1,0/..."  → replace first '/'  → "1,0\..."
//   Karn, numeric-first  "10 U ..."  → replace first ' ' → "10|U ..."
//   Karn, letter-first   "U e' ..."  → prepend before first token → "|U e' ..."
//     (a leading letter means the raw alg had a leading slash, so the implicit
//      slice is BEFORE the first karn token, not after it)
// -------------------------------------------------------
static QString injectSliceIndicatorDisplay(const QString &line, const QString &sliceStr)
{
    if (sliceStr.isEmpty())
        return line; // no indicator to inject

    int lb = line.lastIndexOf('[');
    QString algPart = lb > 0 ? line.left(lb).trimmed() : line.trimmed();
    QString rest = lb > 0 ? "  " + line.mid(lb).trimmed() : QString();

    if (algPart.isEmpty())
        return line;

    // Letter-first → implicit leading slice is before this token
    if (algPart[0].isLetter())
        return sliceStr + algPart + rest;

    // Numeric-first: find the first delimiter among / \ | and space.
    // Order matters: / and \ are consumed by karnify (split), but | survives
    // and will cause a double injection if we search for space first.
    int p = algPart.indexOf('/');
    if (p < 0)
        p = algPart.indexOf('\\');
    if (p < 0)
        p = algPart.indexOf('|');
    if (p < 0)
        p = algPart.indexOf(' ');
    if (p < 0)
        return line; // single-move alg — nowhere to inject

    // If the delimiter is already the indicator, no-op (avoids double injection).
    if (algPart.mid(p, sliceStr.size()) == sliceStr)
        return line;

    return algPart.left(p) + sliceStr + algPart.mid(p + 1) + rest;
}

// -------------------------------------------------------
// onSolverLine
// -------------------------------------------------------
void MainWindow::onSolverLine(QString line)
{
    bool isSolution = line.contains('[') && line.contains(']');
    QString karnLine = line; // default: non-solution lines are unchanged
    QString debugAnn;        // per-alg rating annotation (populated in Step 2)

    if (isSolution)
    {
        // Dedup on the raw WCA alg key (before any display conversion)
        int bracketPos = line.indexOf('[');
        QString algKey = (bracketPos >= 0) ? line.left(bracketPos).trimmed() : line.trimmed();
        if (m_seenSolutions.contains(algKey))
            return;
        m_seenSolutions.insert(algKey);

        // ── Step 1: store CLEAN line for normalisation pass in onSolverDone ─────
        // Must happen before any modification of 'line'.
        m_solutionLinesForRating.append(line);

        // ── Step 2: rating (cubeshape solves only) ────────────────────────────
        // One rateAlg call captures both sliceStart (for display) and FINAL score
        // (stored for the post-solve normalisation pass).  If cubeshape is off,
        // nothing rating-related happens at all — no injection, no indicators.
        QString sliceStr;
        if (m_cubeshapeWasActive)
        {
            bool initial_top_A = false;
            if (!m_posHex.isEmpty())
            {
                QChar first = m_posHex[0];
                initial_top_A = first.isDigit() || first == 'X' || first == 'Y' || first == 'Z';
            }
            double rawFinal = std::numeric_limits<double>::quiet_NaN();
            try
            {
                AlgRating rating = rateAlg(algKey.toStdString(), initial_top_A, 34, 100, 38, 10);
                if (rating.valid)
                {
                    sliceStr = QString::fromStdString(rating.sliceStart);
                    rawFinal = rating.FINAL;
                    if (m_debugOutput)
                    {
                        debugAnn = QString("[DEBUG]  (P1=%1 P2=%2 P3=%3 P4=%4 F=%5  ergo_up=%6 ergo_dn=%7 sl=%8 mv=%9 bon=%10)")
                                       .arg(rating.PHASE1, 0, 'f', 1)
                                       .arg(rating.PHASE2, 0, 'f', 1)
                                       .arg(rating.PHASE3, 0, 'f', 1)
                                       .arg(rating.PHASE4, 0, 'f', 1)
                                       .arg(rating.FINAL, 0, 'f', 2)
                                       .arg(rating.ergo_up, 0, 'f', 1)
                                       .arg(rating.ergo_down, 0, 'f', 1)
                                       .arg(rating.sliceCount)
                                       .arg(rating.movement)
                                       .arg(rating.bonus);
                    }
                }
            }
            catch (...)
            {
            }
            m_sliceIndicators.append(sliceStr); // "" if rateAlg failed
            m_rawFinalScores.append(rawFinal);  // NaN if rateAlg failed
        }

        // ── Step 3: inject indicator into raw display line (cubeshape only) ──
        QString injectedLine = injectSliceIndicatorDisplay(line, sliceStr);

        // ── Step 4: karnify, then re-inject (cubeshape only) ─────────────────
        line = injectedLine;
        karnLine = injectSliceIndicatorDisplay(convertLine(line), sliceStr);

        // Store per-alg annotation (empty if not cubeshape or debug off)
        m_algAnnLines.append(debugAnn);

        // Cache both display versions (clean — no annotation appended to lines)
        m_solutionLines.append(injectedLine);
        m_karnSolutionLines.append(karnLine);

        // Overwrite 'line' so the raw-display path below uses the injected version
        line = injectedLine;
    }

    // Cache into raw and karn line lists (non-solution lines are identical in both)
    m_rawLines.append(line);
    m_karnLines.append(karnLine);

    // Which version to display live?
    QString displayLine = isSolution && chkKarnotation->isChecked() ? karnLine : line;
    if (isSolution)
    {
        QString normalized = applyNormalizeAbf(displayLine);
        displayLine = normalized;

        // Dedup: two raw solutions may normalise to the same display string
        int lbs = displayLine.lastIndexOf('[');
        QString normKey = lbs > 0 ? displayLine.left(lbs).trimmed() : displayLine.trimmed();
        if (m_seenNormalizedAlgs.contains(normKey))
            return;
        m_seenNormalizedAlgs.insert(normKey);
    }

    if (isSolution)
    {
        if (!m_hadFirstSolution && !m_debugOutput)
        {
            m_hadFirstSolution = true;
            m_firstSolutionMs = QDateTime::currentMSecsSinceEpoch();
            // Wipe the "Initializing…" / "searching depth N" / "Flags: …" / "Position: …"
            // lines from both the display and the raw/karn caches.  We keep only entries
            // that are themselves solution lines (contain '[' and ']').
            auto isSol = [](const QString &s)
            { return s.contains('[') && s.contains(']'); };
            m_rawLines.erase(std::remove_if(m_rawLines.begin(), m_rawLines.end(),
                                            [&](const QString &s)
                                            { return !isSol(s); }),
                             m_rawLines.end());
            m_karnLines.erase(std::remove_if(m_karnLines.begin(), m_karnLines.end(),
                                             [&](const QString &s)
                                             { return !isSol(s); }),
                              m_karnLines.end());
            txtOutput->clear();
        }
        btnExpand->setVisible(true);
        btnCopyTerminal->setVisible(true);
        btnFavorites->setVisible(true);
        btnTableMode->setVisible(true);
        // Start idle-fade timer on first reveal; restores to full if a prior solve
        // had left the buttons faded.
        setOutputBtnsOpacity(1.0, 0); // instant snap to full on new results
        if (m_outputIdleTimer)
            m_outputIdleTimer->start(1500);
        {
            bool isAlt = (m_solutionLines.size() % 2 == 0);
            QString col = isAlt ? Theme::solutionAltLight() : Theme::textSolution();
            QTextCursor cur = txtOutput->textCursor();
            cur.movePosition(QTextCursor::End);
            if (!txtOutput->document()->isEmpty())
                cur.insertBlock();
            QTextBlockFormat blkFmt;
            blkFmt.setLineHeight(m_expanded ? 180 : 120, QTextBlockFormat::ProportionalHeight);
            cur.setBlockFormat(blkFmt);
            QTextCharFormat fmt;
            fmt.setForeground(QColor(col));
            fmt.setFontWeight(m_expanded ? QFont::Bold : QFont::Normal);
            fmt.setFontPointSize(m_expanded ? 13 : 10);
            if (m_abidNotation && !OutputConverter::s_abidFontFamily.isEmpty())
            {
                int lb = displayLine.lastIndexOf('[');
                QString algPart = lb > 0 ? displayLine.left(lb).trimmed() : displayLine;
                QString bracketPart = lb > 0 ? "  " + displayLine.mid(lb).trimmed() : QString();
                QTextCharFormat abidFmt = fmt;
                abidFmt.setFontFamily(OutputConverter::s_abidFontFamily);
                abidFmt.setFontPointSize(fmt.fontPointSize() + 2);
                cur.insertText(OutputConverter::abidifyDisplay(algPart), abidFmt);
                if (!bracketPart.isEmpty())
                    cur.insertText(bracketPart, fmt);
            }
            else
            {
                cur.insertText(displayLine, fmt);
            }
            {
                int saved = txtOutput->verticalScrollBar()->value();
                txtOutput->setTextCursor(cur);
                if (m_autoScrollPaused)
                    txtOutput->verticalScrollBar()->setValue(saved);
                else
                    txtOutput->verticalScrollBar()->setValue(
                        txtOutput->verticalScrollBar()->maximum());
            }
            // ── Per-alg debug annotation ──────────────────────────────
            if (m_debugOutput && !debugAnn.isEmpty())
            {
                int saved = txtOutput->verticalScrollBar()->value();
                QTextCursor acur = txtOutput->textCursor();
                acur.movePosition(QTextCursor::End);
                acur.insertBlock();
                QTextCharFormat afmt;
                afmt.setForeground(QColor(Theme::textMuted()));
                afmt.setFontItalic(false);
                afmt.setFontPointSize(m_expanded ? 11 : 9);
                QTextBlockFormat ablkFmt;
                ablkFmt.setLineHeight(120, QTextBlockFormat::ProportionalHeight);
                acur.setBlockFormat(ablkFmt);
                acur.insertText(debugAnn, afmt);
                txtOutput->setTextCursor(acur);
                if (m_autoScrollPaused)
                    txtOutput->verticalScrollBar()->setValue(saved);
                else
                    txtOutput->verticalScrollBar()->setValue(
                        txtOutput->verticalScrollBar()->maximum());
            }
        }
    }
    else
    {
        QTextCursor cur = txtOutput->textCursor();
        cur.movePosition(QTextCursor::End);
        if (!txtOutput->document()->isEmpty())
            cur.insertBlock();
        QTextBlockFormat blkFmt;
        blkFmt.setLineHeight(m_expanded ? 150 : 120, QTextBlockFormat::ProportionalHeight);
        cur.setBlockFormat(blkFmt);
        QTextCharFormat fmt;
        fmt.setForeground(QColor(Theme::textMuted()));
        fmt.setFontPointSize(m_expanded ? 11 : 10);
        cur.insertText(displayLine, fmt);
        {
            int saved = txtOutput->verticalScrollBar()->value();
            txtOutput->setTextCursor(cur);
            if (m_autoScrollPaused)
                txtOutput->verticalScrollBar()->setValue(saved);
            else
                txtOutput->verticalScrollBar()->setValue(
                    txtOutput->verticalScrollBar()->maximum());
        }
    }
}

// -------------------------------------------------------
// onSolverDone
// -------------------------------------------------------
void MainWindow::onSolverDone(int code)
{
    // Re-enable all controls now that solver is done.
    setSolverRunning(false);
    btnSolve->setText("▶  Solve  [Ctrl+Enter]");
    btnSolve->setStyleSheet(""); // revert to stylesheet-defined look
    // Progress bar stays visible in indeterminate mode while ergo ranks
    progressBar->setRange(0, 0); // indeterminate pulse
    progressBar->setVisible(true);

    const int n = m_solutionLines.size();
    double secs = (QDateTime::currentMSecsSinceEpoch() - m_solveStartMs) / 1000.0;
    QString secsStr = QString::number(secs, 'f', 2);

    if (m_stopped)
    {
        QString summary = QString("Stopped — %1 solution%2 found in %3s.")
                              .arg(n)
                              .arg(n == 1 ? "" : "s")
                              .arg(secsStr);
        appendStatusLine(summary);
    }
    else if (code == 0)
    {
        QString summary = QString("Done — %1 solution%2 found in %3s.")
                              .arg(n)
                              .arg(n == 1 ? "" : "s")
                              .arg(secsStr);
        appendStatusLine(summary);
    }
    else
    {
        appendStatusLine("Error (code " + QString::number(code) + ")");
    }

    // ── Ergonomic rating cache (cubeshape solves only) ────────────────────────
    // Scores were already computed per-line in onSolverLine (one rateAlg call each).
    // Here we only normalise and sort — no rateAlg calls.
    if (m_cubeshapeWasActive && !m_rawFinalScores.isEmpty())
    {
        // Collect valid (non-NaN) scores for median computation
        std::vector<double> valid;
        valid.reserve(m_rawFinalScores.size());
        for (double s : m_rawFinalScores)
            if (!std::isnan(s))
                valid.push_back(s);

        // Median-normalise
        double median = 0.0;
        if (!valid.empty())
        {
            std::sort(valid.begin(), valid.end());
            size_t nv = valid.size();
            median = (nv % 2 == 0)
                         ? (valid[nv / 2 - 1] + valid[nv / 2]) / 2.0
                         : valid[nv / 2];
        }

        QVector<QPair<int, double>> indexScores;
        indexScores.reserve(m_rawFinalScores.size());
        for (int i = 0; i < m_rawFinalScores.size(); i++)
        {
            double sc = m_rawFinalScores[i];
            indexScores.append({i, std::isnan(sc) ? sc : sc - median});
        }

        // Sort highest first, NaN last
        std::stable_sort(indexScores.begin(), indexScores.end(),
                         [](const QPair<int, double> &a, const QPair<int, double> &b)
                         {
                             bool aN = std::isnan(a.second), bN = std::isnan(b.second);
                             if (aN && bN)
                                 return false;
                             if (aN)
                                 return false;
                             if (bN)
                                 return true;
                             return a.second > b.second;
                         });
        m_cachedRatedOrder = indexScores;
        m_ratingsValid = true;
        debugLine(QString("ergo: %1 rated, %2 unratable, median=%3")
                      .arg(valid.size())
                      .arg(m_rawFinalScores.size() - (int)valid.size())
                      .arg(median, 0, 'f', 4));
        // Defer the terminal rebuild so the progress bar gets one repaint first
        QTimer::singleShot(0, this, [this]()
                           {
            // Let the indeterminate bar render at least one frame
            QApplication::processEvents(QEventLoop::ExcludeUserInputEvents, 50);
            onRankErgoToggled(true);
            // Let the terminal repaint before building the table
            QApplication::processEvents(QEventLoop::ExcludeUserInputEvents, 50);
            progressBar->setRange(0, 100);
            progressBar->setVisible(false); });
    }

    if (!m_cubeshapeWasActive || m_rawFinalScores.isEmpty())
    {
        // No ergo computation — hide bar immediately
        progressBar->setRange(0, 100);
        progressBar->setVisible(false);
    }

    if (!m_solutionLines.isEmpty())
    {
        if (m_autoScrollPaused)
        {
            // User is browsing — flag it and animate the scroll button
            m_solveFinishedWhilePaused = true;
            // Animate: checkmark for 1.5s, then table icon
            // Animate text color from transparent to green (fade-in the checkmark content only)
            btnScrollToBottom->setText("✓");
            btnScrollToBottom->setToolTip("Done!");
            {
                // Use a QTimer to step the text color alpha from 0 to full
                auto *stepTimer = new QTimer(btnScrollToBottom);
                stepTimer->setInterval(16);
                auto *step = new int(0);
                auto applyCheckStyle = [this](int alpha)
                {
                    btnScrollToBottom->setStyleSheet(QString(
                                                         "QPushButton#btnScrollToBottom {"
                                                         "  background: #1a5c1a; border: 1px solid #2ecc40; border-radius: 16px;"
                                                         "  color: rgba(46,204,64,%1); font-size: 18px; font-weight: bold;"
                                                         "  padding: 0px; margin: 0px; text-align: center; }"
                                                         "QPushButton#btnScrollToBottom:hover { background: #236b23; }")
                                                         .arg(alpha));
                };
                applyCheckStyle(0);
                connect(stepTimer, &QTimer::timeout, this, [this, stepTimer, step, applyCheckStyle]() mutable
                        {
                    *step += 1;
                    int alpha = qMin((int)(*step * 255 / 22), 255); // ~350ms at 16ms steps
                    applyCheckStyle(alpha);
                    if (*step >= 22) { stepTimer->stop(); delete step; } });
                stepTimer->start();
            }

            // After 1.5s: fade out checkmark text, swap to table icon, fade in
            QTimer::singleShot(1500, this, [this]
                               {
                if (!m_solveFinishedWhilePaused) return;
                auto *stepOut = new QTimer(btnScrollToBottom);
                stepOut->setInterval(16);
                auto *stepO = new int(0);
                auto applyCheckFade = [this](int alpha) {
                    btnScrollToBottom->setStyleSheet(QString(
                        "QPushButton#btnScrollToBottom {"
                        "  background: #1a5c1a; border: 1px solid #2ecc40; border-radius: 16px;"
                        "  color: rgba(46,204,64,%1); font-size: 18px; font-weight: bold;"
                        "  padding: 0px; margin: 0px; text-align: center; }"
                        "QPushButton#btnScrollToBottom:hover { background: #236b23; }")
                        .arg(alpha));
                };
                connect(stepOut, &QTimer::timeout, this, [this, stepOut, stepO, applyCheckFade]() mutable {
                    *stepO += 1;
                    int alpha = qMax(255 - (int)(*stepO * 255 / 14), 0); // ~220ms
                    applyCheckFade(alpha);
                    if (*stepO >= 14) {
                        stepOut->stop(); delete stepO;
                        // Now swap to table icon and fade in
                        btnScrollToBottom->setText("⊞");
                        btnScrollToBottom->setToolTip("Switch to table view");
                        auto *stepIn = new QTimer(btnScrollToBottom);
                        stepIn->setInterval(16);
                        auto *stepI = new int(0);
                        auto applyTableStyle = [this](int alpha) {
                            btnScrollToBottom->setStyleSheet(QString(
                                "QPushButton#btnScrollToBottom {"
                                "  background: #2a2a4a; border: 1px solid #7a6aaa; border-radius: 16px;"
                                "  color: rgba(204,170,255,%1); font-size: 18px; font-weight: bold;"
                                "  padding: 0px; margin: 0px; text-align: center; }"
                                "QPushButton#btnScrollToBottom:hover { background: #3a3a6a; }")
                                .arg(alpha));
                        };
                        applyTableStyle(0);
                        connect(stepIn, &QTimer::timeout, this, [this, stepIn, stepI, applyTableStyle]() mutable {
                            *stepI += 1;
                            int alpha = qMin((int)(*stepI * 255 / 22), 255);
                            applyTableStyle(alpha);
                            if (*stepI >= 22) { stepIn->stop(); delete stepI; }
                        });
                        stepIn->start();
                    }
                });
                stepOut->start(); });
        }
        else
        {
            qint64 elapsed = m_hadFirstSolution
                                 ? (QDateTime::currentMSecsSinceEpoch() - m_firstSolutionMs)
                                 : 3000;
            int delay = (elapsed < 3000) ? 400 : 0;
            QTimer::singleShot(delay, this, [this]
                               {
                                   m_tableVisible = true;
                                   txtOutput->setVisible(false);
                                   m_tableContainer->setVisible(true);
                                   btnTableMode->setText("▤");
                                   btnTableMode->setToolTip("Switch to terminal view");
                                   rebuildTable(); });
        }
    }
}

// -------------------------------------------------------
// pushUndoState
// -------------------------------------------------------
void MainWindow::pushUndoState()
{
    m_undoStack.append({cubeWidget->getPositionString()});
    if (m_undoStack.size() > 64)
        m_undoStack.removeFirst();
    btnUndo->setEnabled(true);
    m_redoStack.clear();
    btnRedo->setEnabled(false);
}

// -------------------------------------------------------
// onReset
// -------------------------------------------------------
void MainWindow::onReset()
{
    updateCommand();
}

// -------------------------------------------------------
// keyPressEvent — the global eventFilter handles all routing;
// this is kept only as a fallback for events that slip through.
// -------------------------------------------------------

void MainWindow::appendStatusLine(const QString &msg)
{
    QString col = Theme::textTerminal();
    int savedScroll = txtOutput->verticalScrollBar()->value();
    QTextCursor cur = txtOutput->textCursor();
    cur.movePosition(QTextCursor::End);
    if (!txtOutput->document()->isEmpty())
        cur.insertBlock();
    QTextCharFormat fmt;
    fmt.setForeground(QColor(col));
    fmt.setFontItalic(true);
    fmt.setFontPointSize(m_expanded ? 11 : 9);
    QTextBlockFormat blkFmt;
    blkFmt.setLineHeight(120, QTextBlockFormat::ProportionalHeight);
    cur.setBlockFormat(blkFmt);
    cur.insertText(msg, fmt);
    // Move cursor to end without triggering scroll — use a temp cursor
    QTextCursor endCur = txtOutput->textCursor();
    endCur.movePosition(QTextCursor::End);
    txtOutput->setTextCursor(endCur);
    if (m_autoScrollPaused)
        txtOutput->verticalScrollBar()->setValue(savedScroll);
    else
        txtOutput->verticalScrollBar()->setValue(
            txtOutput->verticalScrollBar()->maximum());
}

void MainWindow::fillNextTableBatch()
{
    if (m_pendingTableRows.isEmpty())
        return;

    const QColor rowA = QColor(Theme::rowAltDark());
    const QColor rowB = QColor(Theme::rowAltLight());
    const QColor textCol = QColor(Theme::textSolution());
    const QColor metaCol = QColor(Theme::textSecondary());
    const int rowH = m_expanded ? 36 : 24;
    const int fontSize = m_expanded ? 15 : 12;
    const bool showErgo = m_cubeshapeWasActive;
    const int metricId = m_metricGroup ? m_metricGroup->checkedId() : 0;
    const int baseColCount = (metricId == 0) ? 3 : (metricId == 1) ? 4
                                                                   : 5;

    const int batchSize = 50;
    int count = qMin(batchSize, m_pendingTableRows.size());

    // Grow the table by exactly the rows we're about to fill — no blank rows.
    int newRowCount = m_tableFilledCount + count;
    m_solutionTable->setRowCount(newRowCount);

    for (int b = 0; b < count; b++)
    {
        const TableRow &r = m_pendingTableRows[b];
        int i = m_tableFilledCount;
        QColor bg = (i % 2 == 0) ? rowA : rowB;

        auto cell = [&](int col, const QString &txt, bool isMeta = false)
        {
            QTableWidgetItem *item = new QTableWidgetItem(txt);
            item->setBackground(bg);
            item->setForeground(isMeta ? metaCol : textCol);
            item->setFlags(Qt::ItemIsEnabled);
            if (m_expanded)
            {
                QFont f = item->font();
                f.setPointSize(fontSize);
                item->setFont(f);
            }
            item->setTextAlignment(Qt::AlignCenter);
            m_solutionTable->setItem(i, col, item);
        };

        QTableWidgetItem *numItem = new QTableWidgetItem(QString::number(i + 1));
        numItem->setBackground(bg);
        numItem->setForeground(metaCol);
        numItem->setFlags(Qt::ItemIsEnabled);
        numItem->setTextAlignment(Qt::AlignCenter);
        numItem->setData(Qt::UserRole, r.rawLine);
        {
            QFont f = numItem->font();
            f.setPointSize(m_expanded ? fontSize - 2 : 10);
            f.setItalic(false);
            numItem->setFont(f);
        }
        m_solutionTable->setItem(i, 0, numItem);

        // Alg column: plain QTableWidgetItem, selectable via SelectableDelegate
        QString algDisplay = (m_abidNotation && !OutputConverter::s_abidFontFamily.isEmpty())
                                 ? OutputConverter::abidifyDisplay(r.alg)
                                 : r.alg;
        QTableWidgetItem *algItem = new QTableWidgetItem(algDisplay);
        algItem->setData(Qt::UserRole, r.alg); // clean alg for Ctrl+C copy
        algItem->setBackground(bg);
        algItem->setForeground(textCol);
        algItem->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
        algItem->setTextAlignment(Qt::AlignVCenter | Qt::AlignLeft);
        if (m_abidNotation && !OutputConverter::s_abidFontFamily.isEmpty())
        {
            QFont f;
            f.setFamily(OutputConverter::s_abidFontFamily);
            f.setPointSize(fontSize + 2);
            algItem->setFont(f);
        }
        else if (m_expanded)
        {
            QFont f = algItem->font();
            f.setPointSize(fontSize);
            algItem->setFont(f);
        }
        m_solutionTable->setItem(i, 1, algItem);

        if (metricId == 0)
        {
            cell(2, QString::number(r.slices), true);
        }
        else if (metricId == 1)
        {
            cell(2, QString::number(r.moves), true);
            cell(3, QString::number(r.slices), true);
        }
        else
        {
            cell(2, QString::number(r.angle), true);
            cell(3, QString::number(r.moves), true);
            cell(4, QString::number(r.slices), true);
        }

        if (showErgo)
        {
            cell(baseColCount, std::isnan(r.ergo) ? "⚠" : QString::number(r.ergo, 'f', 1), true);
        }
        m_solutionTable->setRowHeight(i, rowH);
        m_tableFilledCount++;
    }
    m_pendingTableRows.remove(0, count);
    if (!m_pendingTableRows.isEmpty())
        m_tableFillTimer->start(16); // ~1 frame later
}

void MainWindow::rebuildTable()
{
    if (m_tableFillTimer)
        m_tableFillTimer->stop();
    m_pendingTableRows.clear();
    m_tableFilledCount = 0;
    const bool ergo = m_ratingsValid && m_cubeshapeWasActive;
    const bool showErgo = m_cubeshapeWasActive;
    const int metricId = m_metricGroup ? m_metricGroup->checkedId() : 0;
    // metricId: 0=Slice, 1=Move, 2=Angle
    // Slice: cols = #, Solution, Slices
    // Move:  cols = #, Solution, Moves, Slices
    // Angle: cols = #, Solution, Angle, Moves, Slices
    int baseColCount = (metricId == 0) ? 3 : (metricId == 1) ? 4
                                                             : 5;
    m_solutionTable->setColumnCount(showErgo ? baseColCount + 1 : baseColCount);
    if (metricId == 0)
        m_solutionTable->setHorizontalHeaderLabels(
            showErgo ? QStringList{"#", "Solution", "Slices", "Ergo"}
                     : QStringList{"#", "Solution", "Slices"});
    else if (metricId == 1)
        m_solutionTable->setHorizontalHeaderLabels(
            showErgo ? QStringList{"#", "Solution", "Moves", "Slices", "Ergo"}
                     : QStringList{"#", "Solution", "Moves", "Slices"});
    else
        m_solutionTable->setHorizontalHeaderLabels(
            showErgo ? QStringList{"#", "Solution", "Angle", "Moves", "Slices", "Ergo"}
                     : QStringList{"#", "Solution", "Angle", "Moves", "Slices"});
    m_solutionTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Fixed);
    m_solutionTable->setColumnWidth(0, 72);
    m_solutionTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    for (int c = 2; c < m_solutionTable->columnCount(); c++)
        m_solutionTable->horizontalHeader()->setSectionResizeMode(c, QHeaderView::ResizeToContents);
    m_solutionTable->setRowCount(0);
    debugLine(QString("rebuildTable: solutionLines=%1 karnSolutionLines=%2 "
                      "ratingsValid=%3 cubeshapeWasActive=%4 ergo=%5 metricId=%6 useKarn=%7")
                  .arg(m_solutionLines.size())
                  .arg(m_karnSolutionLines.size())
                  .arg(m_ratingsValid)
                  .arg(m_cubeshapeWasActive)
                  .arg(m_ratingsValid && m_cubeshapeWasActive)
                  .arg(m_metricGroup ? m_metricGroup->checkedId() : 0)
                  .arg(chkKarnotation->isChecked()));
    if (m_solutionLines.isEmpty())
        return;

    auto parseCounts = [](const QString &line, int &moves, int &slices, int &angle)
    {
        moves = 0;
        slices = 0;
        angle = 0;
        int lb = line.lastIndexOf('[');
        int rb = line.lastIndexOf(']');
        if (lb < 0 || rb < 0)
            return;
        QString bracket = line.mid(lb + 1, rb - lb - 1);
        QStringList parts = bracket.split('|');
        if (parts.size() == 1)
        {
            slices = parts[0].trimmed().toInt();
        }
        else if (parts.size() >= 2)
        {
            slices = parts[0].trimmed().toInt();
            moves = parts[1].trimmed().toInt();
        }
        if (parts.size() >= 3)
            angle = parts[2].trimmed().toInt();
    };

    struct Row
    {
        QString alg;
        int moves;
        int slices;
        int angle;
        double ergo;
        QString rawLine;
    };
    QVector<Row> rows;

    bool useKarn = chkKarnotation->isChecked();
    const QStringList &displayLines = useKarn ? m_karnSolutionLines : m_solutionLines;

    QSet<QString> seenTableAlgs;
    if (showErgo && m_ratingsValid)
    {
        for (auto &[idx, score] : m_cachedRatedOrder)
        {
            if (idx < 0 || idx >= displayLines.size())
                continue;
            const QString dline = applyNormalizeAbf(displayLines[idx]);
            int mv, sl, ang;
            parseCounts(dline, mv, sl, ang);
            int lb = dline.lastIndexOf('[');
            QString alg = lb > 0 ? dline.left(lb).trimmed() : dline.trimmed();
            if (seenTableAlgs.contains(alg))
                continue;
            seenTableAlgs.insert(alg);
            rows.append({alg, mv, sl, ang, score, dline}); // dline = full display line with brackets
        }
    }
    else
    {
        for (int di = 0; di < displayLines.size(); di++)
        {
            const QString line = applyNormalizeAbf(displayLines[di]);
            int mv, sl, ang;
            parseCounts(line, mv, sl, ang);
            int lb = line.lastIndexOf('[');
            QString alg = lb > 0 ? line.left(lb).trimmed() : line.trimmed();
            if (seenTableAlgs.contains(alg))
                continue;
            seenTableAlgs.insert(alg);
            rows.append({alg, mv, sl, ang, 0.0, line}); // line = full display line with brackets
        }
    }

    // Sort: ergo rank already baked into row order from cache; for non-ergo sort by moves/slices
    if (!(ergo && showErgo && m_ratingsValid))
        std::stable_sort(rows.begin(), rows.end(), [](const Row &a, const Row &b)
                         {
            if (a.slices != b.slices) return a.slices < b.slices;
            return a.moves < b.moves; });

    const QColor rowA = QColor(Theme::rowAltDark());
    const QColor rowB = QColor(Theme::rowAltLight());
    const QColor textCol = QColor(Theme::textSolution());
    const QColor metaCol = QColor(Theme::textSecondary());
    const int rowH = m_expanded ? 36 : 24;
    const int fontSize = m_expanded ? 15 : 12;

    // Install SelectableDelegate on the alg column so items are copy-pasteable
    // via a read-only QLineEdit editor — without using QLabel cell widgets which
    // grab mouse events and break table scrolling.
    m_solutionTable->setItemDelegateForColumn(1, new SelectableDelegate(m_solutionTable));

    // Helper: build a QTableWidgetItem for the alg column
    auto makeAlgItem = [&](const Row &r, const QColor &bg) -> QTableWidgetItem *
    {
        QString algDisplay = (m_abidNotation && !OutputConverter::s_abidFontFamily.isEmpty())
                                 ? OutputConverter::abidifyDisplay(r.alg)
                                 : r.alg;
        QTableWidgetItem *item = new QTableWidgetItem(algDisplay);
        item->setData(Qt::UserRole, r.alg); // clean alg for Ctrl+C copy
        item->setBackground(bg);
        item->setForeground(textCol);
        item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
        item->setTextAlignment(Qt::AlignVCenter | Qt::AlignLeft);
        if (m_abidNotation && !OutputConverter::s_abidFontFamily.isEmpty())
        {
            QFont f;
            f.setFamily(OutputConverter::s_abidFontFamily);
            f.setPointSize(fontSize + 2);
            item->setFont(f);
        }
        else if (m_expanded)
        {
            QFont f = item->font();
            f.setPointSize(fontSize);
            item->setFont(f);
        }
        return item;
    };

    const int firstBatch = qMin(50, rows.size());
    // Only pre-allocate rows that will actually be filled in this pass.
    // Remaining rows are added incrementally by fillNextTableBatch so the
    // scrollbar never extends into blank, empty-cell territory.
    m_solutionTable->setRowCount(firstBatch);
    for (int i = 0; i < firstBatch; i++)
    {
        const Row &r = rows[i];
        QColor bg = (i % 2 == 0) ? rowA : rowB;

        auto cell = [&](int col, const QString &txt, bool isMeta = false)
        {
            QTableWidgetItem *item = new QTableWidgetItem(txt);
            item->setBackground(bg);
            item->setForeground(isMeta ? metaCol : textCol);
            item->setFlags(Qt::ItemIsEnabled); // not selectable
            if (m_expanded)
            {
                QFont f = item->font();
                f.setPointSize(fontSize);
                item->setFont(f);
            }
            item->setTextAlignment(Qt::AlignCenter);
            m_solutionTable->setItem(i, col, item);
        };

        // SI no column — fixed font, no italic
        QTableWidgetItem *numItem = new QTableWidgetItem(QString::number(i + 1));
        numItem->setBackground(bg);
        numItem->setForeground(metaCol);
        numItem->setFlags(Qt::ItemIsEnabled);
        numItem->setTextAlignment(Qt::AlignCenter);
        numItem->setData(Qt::UserRole, r.rawLine);
        {
            QFont f = numItem->font();
            f.setPointSize(m_expanded ? fontSize - 2 : 10);
            f.setItalic(false);
            numItem->setFont(f);
        }
        m_solutionTable->setItem(i, 0, numItem);

        m_solutionTable->setItem(i, 1, makeAlgItem(r, bg));

        if (metricId == 0)
        {
            cell(2, QString::number(r.slices), true);
        }
        else if (metricId == 1)
        {
            cell(2, QString::number(r.moves), true);
            cell(3, QString::number(r.slices), true);
        }
        else
        {
            cell(2, QString::number(r.angle), true);
            cell(3, QString::number(r.moves), true);
            cell(4, QString::number(r.slices), true);
        }
        if (showErgo)
        {
            if (std::isnan(r.ergo))
                cell(baseColCount, "⚠", true);
            else
                cell(baseColCount, QString::number(r.ergo, 'f', 1), true);
        }
        m_solutionTable->setRowHeight(i, rowH);
    }
    // Store overflow rows for deferred fill
    m_pendingTableRows.clear();
    m_tableFilledCount = firstBatch;
    for (int i = firstBatch; i < rows.size(); i++)
    {
        const Row &r = rows[i];
        m_pendingTableRows.append({r.alg, r.moves, r.slices, r.angle, r.ergo, r.rawLine});
    }
    if (!m_pendingTableRows.isEmpty())
    {
        if (!m_tableFillTimer)
        {
            m_tableFillTimer = new QTimer(this);
            m_tableFillTimer->setSingleShot(true);
            connect(m_tableFillTimer, &QTimer::timeout, this, &MainWindow::fillNextTableBatch);
        }
        m_tableFillTimer->start(16);
    }
}

void MainWindow::resizeEvent(QResizeEvent *event)
{
    QMainWindow::resizeEvent(event);
    applyZoom();
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    // Persist favorites, run config, and display options on exit.
    saveSettings();
    QMainWindow::closeEvent(event);
}

void MainWindow::applyZoom()
{
    if (!m_zoomProxy || !m_zoomScene || !m_zoomView)
        return;
    QTransform t;
    t.scale(m_zoomScale, m_zoomScale);
    m_zoomView->setTransform(t);
    QSizeF inner(width() / m_zoomScale, height() / m_zoomScale);
    m_zoomProxy->resize(inner);
    m_zoomScene->setSceneRect(0, 0, inner.width(), inner.height());

    // Treat logical width exactly like a browser viewport:
    // zoom in = smaller logical width = smaller left panel
    // zoom out = larger logical width = larger left panel
    int logicalW = static_cast<int>(inner.width());
    int leftW = qBound(260, 260 + (logicalW - 760) / 5, 420);
    leftScroll->setFixedWidth(leftW);
    if (auto *lc = leftScroll->widget())
        lc->setFixedWidth(leftW - 4);
}

void MainWindow::keyPressEvent(QKeyEvent *event)
{
    QMainWindow::keyPressEvent(event);
}

// -------------------------------------------------------
// onCopy
// -------------------------------------------------------
void MainWindow::onCopy()
{
    QApplication::clipboard()->setText(txtCommand->text());
    appendStatusLine("Copied to clipboard!");
}

// -------------------------------------------------------
// onRankErgoToggled
// -------------------------------------------------------
void MainWindow::onRankErgoToggled(bool checked)
{
    if (!checked)
    {
        rebuildTerminalView();
        appendStatusLine("Done.");
        if (m_tableVisible)
            rebuildTable();
        return;
    }
    if (m_solutionLines.isEmpty())
        return;
    // Ergo rating is only meaningful for cubeshape solves
    if (!m_cubeshapeWasActive)
        return;
    if (!m_ratingsValid)
        return;
    lblStatus->setText("Rating algorithms…");

    const bool useKarn = chkKarnotation->isChecked();
    const QStringList &displaySols = useKarn ? m_karnSolutionLines : m_solutionLines;

    txtOutput->clear();
    QTextCursor cur(txtOutput->document());
    bool firstBlock = true;

    // insertLine: plain text, no abid transformation (used for status / non-sol lines)
    auto insertLine = [&](const QString &text, const QString &color, bool bold, int ptSize, int lineH)
    {
        if (!firstBlock)
            cur.insertBlock();
        firstBlock = false;
        QTextBlockFormat blkFmt;
        blkFmt.setLineHeight(lineH, QTextBlockFormat::ProportionalHeight);
        cur.setBlockFormat(blkFmt);
        QTextCharFormat fmt;
        fmt.setForeground(QColor(color));
        fmt.setFontWeight(bold ? QFont::Bold : QFont::Normal);
        fmt.setFontPointSize(ptSize);
        cur.insertText(text, fmt);
    };

    // insertSolLine: solution line with optional abid font on the alg portion
    auto insertSolLine = [&](const QString &algPart, const QString &suffix,
                             const QString &color, bool bold, int ptSize, int lineH)
    {
        if (!firstBlock)
            cur.insertBlock();
        firstBlock = false;
        QTextBlockFormat blkFmt;
        blkFmt.setLineHeight(lineH, QTextBlockFormat::ProportionalHeight);
        cur.setBlockFormat(blkFmt);
        QTextCharFormat fmt;
        fmt.setForeground(QColor(color));
        fmt.setFontWeight(bold ? QFont::Bold : QFont::Normal);
        fmt.setFontPointSize(ptSize);
        if (m_abidNotation && !OutputConverter::s_abidFontFamily.isEmpty())
        {
            QTextCharFormat abidFmt = fmt;
            abidFmt.setFontFamily(OutputConverter::s_abidFontFamily);
            abidFmt.setFontPointSize(ptSize + 2);
            cur.insertText(OutputConverter::abidifyDisplay(algPart), abidFmt);
            if (!suffix.isEmpty())
                cur.insertText(suffix, fmt);
        }
        else
        {
            cur.insertText(algPart + suffix, fmt);
        }
    };

    // ── Debug lines ────────────────────────────────────────────
    if (m_debugOutput && !m_debugBuffer.isEmpty())
    {
        for (const QString &dbg : std::as_const(m_debugBuffer))
            insertLine(dbg, Theme::textMuted(), false, m_expanded ? 11 : 9, 120);
    }

    // Non-solution lines first (from the appropriate display list)
    const QStringList &displayLines = useKarn ? m_karnLines : m_rawLines;
    for (const QString &line : std::as_const(displayLines))
    {
        bool isSol = line.contains('[') && line.contains(']');
        if (!isSol)
        {
            insertLine(line, Theme::textMuted(), false, m_expanded ? 11 : 10, m_expanded ? 150 : 120);
        }
    }

    // Rated solution lines — read from cache (already sorted, no re-rating)
    int solIdx = 0;
    QSet<QString> seenNormAlgs;
    for (auto &[idx, score] : m_cachedRatedOrder)
    {
        if (solIdx % 20 == 0)
            QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
        if (idx < 0 || idx >= displaySols.size())
        {
            solIdx++;
            continue;
        }
        const QString dline = applyNormalizeAbf(displaySols[idx]);
        int lb = dline.lastIndexOf('[');
        QString displayAlg = lb > 0 ? dline.left(lb).trimmed() : dline.trimmed();
        if (seenNormAlgs.contains(displayAlg))
        {
            solIdx++;
            continue;
        }
        seenNormAlgs.insert(displayAlg);
        QString bracketPart = lb > 0 ? dline.mid(lb).trimmed() : QString();

        bool isAlt = (solIdx % 2 == 1);
        QString col = isAlt ? Theme::solutionAltLight() : Theme::textSolution();

        if (std::isnan(score))
        {
            QString suffix = (bracketPart.isEmpty() ? QString() : "  " + bracketPart) + "  (⚠)";
            insertSolLine(displayAlg, suffix, "#cc2020", m_expanded, m_expanded ? 13 : 10, m_expanded ? 180 : 120);
        }
        else
        {
            QString suffix = (bracketPart.isEmpty() ? QString() : "  " + bracketPart) + QString("  (%1)").arg(score, 0, 'f', 2);
            insertSolLine(displayAlg, suffix, col, m_expanded, m_expanded ? 13 : 10, m_expanded ? 180 : 120);
        }
        // Per-alg debug annotation right after the alg
        if (m_debugOutput && idx < m_algAnnLines.size() && !m_algAnnLines[idx].isEmpty())
            insertLine(m_algAnnLines[idx], Theme::textMuted(), false, m_expanded ? 11 : 9, 120);
        solIdx++;
    }
    appendStatusLine(QString("Ranked %1 algs by ergonomics.").arg((int)m_cachedRatedOrder.size()));
    txtOutput->verticalScrollBar()->setValue(0);
    if (m_tableVisible)
        rebuildTable();
}

// -------------------------------------------------------
// eventFilter — installed on qApp so it intercepts key events
// from every widget (spinboxes, txtDepths, etc.) before they
// are delivered.  Three responsibilities:
//   1. Ctrl+C  → copy (Abid-notation aware); never stops the solver.
//   2. Cube letter shortcuts → route to cubeWidget from any focus.
//   3. txtDepths digit → auto-enable "Specific depths" checkbox.
// -------------------------------------------------------
void MainWindow::showAboutModal()
{
    QWidget *central = this->centralWidget();

    QWidget *overlay = new QWidget(central);
    overlay->setGeometry(central->rect());
    overlay->setStyleSheet("background:rgba(0,0,0,160);");
    overlay->show();
    overlay->raise();

    QString modalBg = Theme::primaryBg();
    QString modalBorder = Theme::borderGroup();
    QWidget *card = new QWidget(overlay);
    card->setObjectName("aboutCard");
    card->setFixedWidth(480);
    card->setStyleSheet(QString(
                            "QWidget#aboutCard { background:%1; border:1px solid %2; border-radius:10px; }")
                            .arg(modalBg, modalBorder));

    QVBoxLayout *lay = new QVBoxLayout(card);
    lay->setContentsMargins(28, 24, 28, 24);
    lay->setSpacing(10);

    QString textPrimary = Theme::textPrimary();
    QString textBody = Theme::textSecondary();
    QLabel *title = new QLabel("About Croissant");
    title->setStyleSheet(QString("font-size:16px;font-weight:bold;color:%1;background:transparent;").arg(textPrimary));
    QTextBrowser *body = new QTextBrowser();
    body->setReadOnly(true);
    body->setFrameShape(QFrame::NoFrame);
    body->setOpenLinks(false);
    body->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    body->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::LinksAccessibleByMouse);
    body->setStyleSheet(QString(
                            "QTextBrowser { background:transparent; border:none; color:%1; }")
                            .arg(textBody));
    body->document()->setDefaultStyleSheet(
        QString("body, span, p { font-size:12px; color:%1; line-height:1.7; }"
                "a { color:%2; }"
                "li { margin-bottom:2px; }")
            .arg(textBody, Theme::linkColor()));
    QString lnk = Theme::linkColor();
    QString aboutBody = QString(
                            "<span style='color:%1;font-size:12px;line-height:1.7;'>"
                            "This program stemmed from the optimal Square-1 solver by "
                            "<a href='https://www.jaapsch.net/puzzles/' style='color:%2;'>Jaap Scherphuis</a>."
                            "<br><br>"
                            "v2 was created by Michael Gottlieb "
                            "(<a href='https://github.com/qqwref' style='color:%2;'>GitHub</a>, "
                            "<a href='https://www.worldcubeassociation.org/persons/2006GOTT01' style='color:%2;'>WCA</a>), "
                            "who rewrote the solver with significant improvements and optimisations."
                            "<br><br>Read the old documentations <a href='read_old_docs' style='color:%2;'>here</a>. Note that it is largely not applicable within v3."
                            "<br><br>This is the official <b style='color:%3;'>v3</b>. New in v3:"
                            "<ul style='margin:4px 0 4px 16px;padding:0;color:%4;'>"
                            "<li>Actual graphical UI</li>"
                            "<li>Ability to generate a solution from a specific angle</li>"
                            "<li>Improved karnotation support</li>"
                            "<li>Algorithm ergonomics rater</li>"
                            "</ul>"
                            "v3 is created by "
                            "<a href='https://www.worldcubeassociation.org/persons/2024ASHR02' style='color:%2;'>Abid Ibn Ashraf</a>"
                            " and "
                            "<a href='https://www.worldcubeassociation.org/persons/2023MAOS01' style='color:%2;'>Matt Mao</a>."
                            "</span>")
                            .arg(textBody, lnk, textPrimary, Theme::textMuted());
    body->setHtml(aboutBody);
    // Enable clicking the in-text link to open the ReadDocs popup
    connect(body, &QTextBrowser::anchorClicked, this, [this](const QUrl &url)
            {
        QString link = url.toString();
            {
                if (link == "read_old_docs") {
                    showReadDocsPopup();
                } else {
                    QDesktopServices::openUrl(QUrl(link));
                }
            }; });

    lay->addWidget(title);
    lay->addWidget(body);

    // Measure the rendered document height at the card's content width.
    // QTextBrowser's own document is accurate here because our default stylesheet
    // (line-height, font-size, list margins) is applied before we measure —
    // unlike creating a bare QTextDocument with only setDefaultFont().
    int cw = qMin(480, central->width() - 40);
    int contentW = cw - lay->contentsMargins().left() - lay->contentsMargins().right();
    body->document()->setTextWidth(contentW);
    int bodyH = static_cast<int>(std::ceil(body->document()->size().height()));
    int totalH = lay->contentsMargins().top() + title->sizeHint().height() + lay->spacing() + bodyH + lay->contentsMargins().bottom() + 4; // small fudge for descenders / border
    int ch = qMin(totalH, central->height() - 40);
    card->setFixedSize(cw, ch);
    card->show();

    auto centerCard = [overlay, card]()
    {
        overlay->setGeometry(overlay->parentWidget()->rect());
        int cx = (overlay->width() - card->width()) / 2;
        int cy = (overlay->height() - card->height()) / 2;
        card->move(cx, cy);
    };
    centerCard();
    card->raise();

    // Watch the centralWidget for resize events
    struct Filter : public QObject
    {
        QWidget *overlay;
        QWidget *card;
        std::function<void()> center;
        Filter(QWidget *o, QWidget *c, std::function<void()> fn)
            : QObject(o), overlay(o), card(c), center(fn) {}
        bool eventFilter(QObject *watched, QEvent *e) override
        {
            if (e->type() == QEvent::Resize && watched == overlay->parentWidget())
            {
                center();
                return false;
            }
            if (e->type() == QEvent::MouseButtonPress && watched == overlay)
            {
                QMouseEvent *me = static_cast<QMouseEvent *>(e);
                if (!card->geometry().contains(me->pos()))
                {
                    overlay->deleteLater();
                    return true;
                }
            }
            return false;
        }
    };

    Filter *f = new Filter(overlay, card, centerCard);
    central->installEventFilter(f); // watches parent for resize
    overlay->installEventFilter(f); // watches overlay for click-outside
}

// Load a document from docs/ directory, searching appDir first then CWD
QString MainWindow::loadDocText(const QString &fileName)
{
    auto readPath = [](const QString &p) -> QString
    {
        QFile f(p);
        if (!f.exists())
            return QString();
        if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
            return QString();
        QString s = QString::fromUtf8(f.readAll());
        f.close();
        return s;
    };

    QString appDir = QCoreApplication::applicationDirPath();
    QDir cwd = QDir::current();

    QList<QString> candidates;
    candidates << cwd.filePath("docs/" + fileName);
    candidates << appDir + "/docs/" + fileName;
    candidates << appDir + "/../docs/" + fileName;
    // ascend from current dir up to 6 levels looking for docs/
    QDir d = cwd;
    for (int i = 0; i < 6; ++i)
    {
        if (!d.cdUp())
            break;
        candidates << d.filePath("docs/" + fileName);
    }

    for (const QString &p : candidates)
    {
        QString s = readPath(p);
        if (!s.isEmpty())
            return s;
    }
    // Final hard fallback: root/docs if present
    QString rootLike = QDir::rootPath() + "/docs/" + fileName;
    QString s = readPath(rootLike);
    if (!s.isEmpty())
        return s;
    return QString();
}

// Read documents popup – shows sq1opt.txt with an inline link to the old-doc
void MainWindow::showReadDocsPopup()
{
    QWidget *central = this->centralWidget();
    QWidget *overlay = new QWidget(central);
    overlay->setObjectName("docsOverlay");
    overlay->setProperty("isDocsOverlay", true);
    overlay->setGeometry(central->rect());
    overlay->setStyleSheet("background:rgba(0,0,0,160);");
    overlay->show();
    overlay->raise();

    QString modalBg = Theme::primaryBg();
    QString modalBorder = Theme::borderGroup();
    QString textColor = Theme::textMuted();
    QString titleColor = Theme::textPrimary();

    QWidget *card = new QWidget(overlay);
    card->setObjectName("docsCard");
    card->setStyleSheet(QString(
                            "QWidget#docsCard { background:%1; border:1px solid %2; border-radius:10px; }")
                            .arg(modalBg, modalBorder));

    QVBoxLayout *lay = new QVBoxLayout(card);
    lay->setContentsMargins(24, 16, 24, 16);
    lay->setSpacing(6);

    QLabel *titleLbl = new QLabel("Sq1opt v2 Documentation");
    titleLbl->setStyleSheet(QString(
                                "font-size:14px;font-weight:bold;color:%1;background:transparent;")
                                .arg(titleColor));
    lay->addWidget(titleLbl);

    QString content = loadDocText("sq1opt.txt");
    if (content.isEmpty())
        content = "Could not load sq1opt.txt";

    // Build HTML line by line so text reflows to card width (no horizontal
    // scroll) while preserving leading-space indentation via &nbsp;.
    QString html;
    for (const QString &line : content.split('\n'))
    {
        QString esc = line.toHtmlEscaped();
        int spaces = 0;
        while (spaces < esc.size() && esc[spaces] == ' ')
            ++spaces;
        if (spaces > 0)
            esc = QString("&nbsp;").repeated(spaces) + esc.mid(spaces);
        esc.replace("sq1opt_old.txt",
                    "<a href='open_old_docs' style='color:#7abfe8;'>sq1opt_old.txt</a>");
        html += "<p style='margin:0;padding:0;'>" + esc + "</p>";
    }

    QTextBrowser *tb = new QTextBrowser(card);
    tb->setReadOnly(true);
    tb->setFrameShape(QFrame::NoFrame);
    tb->setOpenLinks(false);
    tb->setLineWrapMode(QTextEdit::WidgetWidth);
    tb->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    tb->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::LinksAccessibleByMouse);
    tb->setStyleSheet(QString(
                          "QTextBrowser { background:transparent; border:none; color:%1; }")
                          .arg(textColor));
    tb->document()->setDefaultStyleSheet(
        QString("body, p { font-family:monospace; font-size:12px; color:%1; line-height:1.4; }"
                "a { color:#7abfe8; }")
            .arg(textColor));
    tb->setHtml("<body>" + html + "</body>");

    connect(tb, &QTextBrowser::anchorClicked, this, [this](const QUrl &url)
            {
                if (url.toString() == "open_old_docs") showOldDocsPopup();
                else QDesktopServices::openUrl(url); });

    lay->addWidget(tb, 1);

    card->show();
    auto centerCard = [overlay, card, central]()
    {
        overlay->setGeometry(overlay->parentWidget()->rect());
        int cw = qMin(640, central->width() - 40);
        int ch = qMin(520, central->height() - 40);
        card->setFixedSize(cw, ch);
        card->move((overlay->width() - card->width()) / 2,
                   (overlay->height() - card->height()) / 2);
    };
    centerCard();
    card->raise();

    // Event filter: resize re-centres the card; click OUTSIDE the card closes
    // all docs modals. The card-geometry check is critical — without it any
    // click that bubbles up from a non-interactive child would close the modal.
    struct F : public QObject
    {
        QWidget *overlay;
        QWidget *card;
        std::function<void()> fn;
        F(QWidget *o, QWidget *c, std::function<void()> f)
            : QObject(o), overlay(o), card(c), fn(f) {}
        bool eventFilter(QObject *w, QEvent *e) override
        {
            if (e->type() == QEvent::Resize && w == overlay->parentWidget())
            {
                fn();
                return false;
            }
            if (e->type() == QEvent::MouseButtonPress && w == overlay)
            {
                QMouseEvent *me = static_cast<QMouseEvent *>(e);
                if (!card->geometry().contains(me->pos()))
                    overlay->deleteLater();
                return true;
            }
            return false;
        }
    };
    F *f = new F(overlay, card, centerCard);
    central->installEventFilter(f); // watches parent for resize
    overlay->installEventFilter(f); // watches overlay for click-outside
}

// Old docs popup – shows sq1opt_old.txt
void MainWindow::showOldDocsPopup()
{
    QWidget *central = this->centralWidget();
    QWidget *overlay = new QWidget(central);
    overlay->setObjectName("docsOverlay");
    overlay->setProperty("isDocsOverlay", true);
    overlay->setGeometry(central->rect());
    overlay->setStyleSheet("background:rgba(0,0,0,160);");
    overlay->show();
    overlay->raise();

    QString modalBg = Theme::primaryBg();
    QString modalBorder = Theme::borderGroup();
    QString textColor = Theme::textMuted();
    QString titleColor = Theme::textPrimary();

    QWidget *card = new QWidget(overlay);
    card->setObjectName("docsOldCard");
    card->setStyleSheet(QString(
                            "QWidget#docsOldCard { background:%1; border:1px solid %2; border-radius:10px; }")
                            .arg(modalBg, modalBorder));

    QVBoxLayout *lay = new QVBoxLayout(card);
    lay->setContentsMargins(24, 16, 24, 16);
    lay->setSpacing(6);

    QLabel *titleLbl = new QLabel("Sq1opt v1 Documentation");
    titleLbl->setStyleSheet(QString(
                                "font-size:14px;font-weight:bold;color:%1;background:transparent;")
                                .arg(titleColor));
    lay->addWidget(titleLbl);

    QTextBrowser *tb = new QTextBrowser(card);
    tb->setReadOnly(true);
    tb->setFrameShape(QFrame::NoFrame);
    tb->setOpenLinks(false);
    tb->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::LinksAccessibleByMouse);
    tb->setStyleSheet(QString(
                          "QTextBrowser { background:transparent; border:none; color:%1; }")
                          .arg(textColor));
    tb->document()->setDefaultStyleSheet(
        QString("body, pre { font-family:monospace; font-size:12px; color:%1; }").arg(textColor));

    QString oldContent = loadDocText("sq1opt_old.txt");
    if (oldContent.isEmpty())
        tb->setPlainText("Could not load sq1opt_old.txt");
    else
        tb->setPlainText(oldContent);

    lay->addWidget(tb, 1);

    card->show();
    auto centerCard = [overlay, card, central]()
    {
        overlay->setGeometry(overlay->parentWidget()->rect());
        int cw = qMin(640, central->width() - 40);
        int ch = qMin(520, central->height() - 40);
        card->setFixedSize(cw, ch);
        card->move((overlay->width() - card->width()) / 2,
                   (overlay->height() - card->height()) / 2);
    };
    centerCard();
    card->raise();

    struct F : public QObject
    {
        QWidget *overlay;
        QWidget *card;
        std::function<void()> fn;
        F(QWidget *o, QWidget *c, std::function<void()> f)
            : QObject(o), overlay(o), card(c), fn(f) {}
        bool eventFilter(QObject *w, QEvent *e) override
        {
            if (e->type() == QEvent::Resize && w == overlay->parentWidget())
            {
                fn();
                return false;
            }
            if (e->type() == QEvent::MouseButtonPress && w == overlay)
            {
                QMouseEvent *me = static_cast<QMouseEvent *>(e);
                if (!card->geometry().contains(me->pos()))
                    overlay->deleteLater();
                return true;
            }
            return false;
        }
    };
    F *f = new F(overlay, card, centerCard);
    central->installEventFilter(f); // watches parent for resize
    overlay->installEventFilter(f); // watches overlay for click-outside
}

// Applies the normalize-ABF display transform to a single raw WCA alg line.
// Only touches the alg portion; the bracket is preserved unchanged.
// mode: 0=Both 1=PreABF 2=PostABF 3=None
QString MainWindow::applyNormalizeAbf(const QString &rawAlgLine) const
{
    if (m_normalizeAbfMode == 3)
        return rawAlgLine;

    // 1. Separate the length bracket (e.g., " [8]")
    int lb = rawAlgLine.lastIndexOf('[');
    QString algPart = (lb > 0) ? rawAlgLine.left(lb).trimmed() : rawAlgLine.trimmed();
    QString bracketPart = (lb > 0) ? "  " + rawAlgLine.mid(lb).trimmed() : QString();

    // 2. Locate first and last delimiters (/, \, |, or spaces)
    static QRegularExpression sepRegex("[/\\\\|\\s]");
    int firstSep = algPart.indexOf(sepRegex);
    int lastSep = algPart.lastIndexOf(sepRegex);

    // 3. Define the normalization logic for a single block (e.g., "1,-3" or "12")
    auto normLambda = [](QString block) -> QString
    {
        auto n = [](int v)
        { return ((v % 3 + 3) % 3 == 2) ? -1 : (v % 3 + 3) % 3; };
        static QRegularExpression moveRegex("(-?\\d)(,?)(-?\\d)");
        QRegularExpressionMatch m = moveRegex.match(block);
        if (m.hasMatch())
        {
            return block.replace(m.captured(0), QString("%1%2%3").arg(n(m.captured(1).toInt())).arg(m.captured(2)).arg(n(m.captured(3).toInt())));
        }
        return block;
    };

    // 4. Split, normalize, and add back together
    bool normPre = (m_normalizeAbfMode == 0 || m_normalizeAbfMode == 1);
    bool normPost = (m_normalizeAbfMode == 0 || m_normalizeAbfMode == 2);

    if (firstSep == -1)
    {
        // Only one move in the string
        QString normalized = normLambda(algPart);
        if (normalized == "0,0" || normalized == "00")
            return bracketPart;
        return normalized + bracketPart;
    }

    QString first = algPart.left(firstSep);
    QString middle = algPart.mid(firstSep, lastSep - firstSep + 1); // Includes the delimiters
    QString last = algPart.mid(lastSep + 1);

    bool omitFirst = false;
    bool omitLast = false;

    if (normPre)
    {
        first = normLambda(first);
        if (first == "0,0" || first == "00")
            omitFirst = true;
    }
    if (normPost)
    {
        last = normLambda(last);
        if (last == "0,0" || last == "00")
            omitLast = true;
    }

    if (omitFirst)
    {
        // Drop whitespace only — preserve slice indicators (/ \ |)
        if (!middle.isEmpty() && middle[0].isSpace())
            middle = middle.mid(1);
        first.clear();
    }
    if (omitLast)
    {
        // Drop trailing whitespace only — preserve slice indicators (/ \ |)
        if (!middle.isEmpty() && middle.back().isSpace())
            middle.chop(1);
        last.clear();
    }

    return first + middle + last + bracketPart;
}

QString MainWindow::convertLine(const QString &rawLine)
{
    int lb = rawLine.lastIndexOf('[');
    int rb = rawLine.lastIndexOf(']');
    if (lb < 0 || rb < 0)
        return rawLine;

    QString algPart = rawLine.left(lb).trimmed();
    QString bracketPart = rawLine.mid(lb).trimmed();

    std::string converted;
    if (m_smartKarn && !m_cubeshapeWasActive)
    {
        converted = karnifycs(algPart.toStdString(), m_posHex.toStdString(), chkGenerator->isChecked());
    }
    else
    {
        converted = karnify(algPart.toStdString());
    }
    return QString::fromStdString(converted) + "  " + bracketPart;
}

void MainWindow::openSidebar()
{
    if (m_sidebarOpen)
        return;
    m_sidebarOpen = true;

    QWidget *central = this->centralWidget();

    m_sidebarOverlay = new QWidget(central);
    m_sidebarOverlay->setGeometry(central->rect());
    m_sidebarOverlay->setStyleSheet("background: rgba(0,0,0,120);");
    m_sidebarOverlay->show();
    m_sidebarOverlay->raise();

    QString sidebarBg = Theme::sidebarBg();
    QString sidebarBorder = Theme::sidebarBorder();
    QString textPrimary = Theme::textPrimary();
    QString textMuted = Theme::textMuted();
    QString hoverBg = Theme::hoverBg();
    QString btnBg = Theme::buttonBg();
    QString btnBorder = Theme::buttonBorder();

    m_sidebar = new QWidget(m_sidebarOverlay);
    m_sidebar->setFixedWidth(220);
    m_sidebar->setStyleSheet(QString(
                                 "QWidget { background: %1; border-right: 2px solid %2; }")
                                 .arg(sidebarBg, sidebarBorder));
    m_sidebar->setGeometry(-220, 0, 220, central->height());

    QVBoxLayout *slay = new QVBoxLayout(m_sidebar);
    slay->setContentsMargins(0, 0, 0, 0);
    slay->setSpacing(0);

    // Header
    QWidget *sHeader = new QWidget();
    sHeader->setFixedHeight(52);
    sHeader->setStyleSheet(QString("QWidget { background: %1; border-bottom: 1px solid %2; border-right: none; }").arg(Theme::darkBg(), sidebarBorder));
    QHBoxLayout *shLay = new QHBoxLayout(sHeader);
    shLay->setContentsMargins(16, 0, 12, 0);
    QLabel *sTitle = new QLabel("Menu");
    sTitle->setStyleSheet(QString("font-size:15px;font-weight:bold;color:%1;background:transparent;border:none;").arg(textPrimary));
    QPushButton *closeBtn = new QPushButton("✕");
    closeBtn->setFixedSize(26, 26);
    closeBtn->setStyleSheet(QString(
                                "QPushButton { background:%1; border:1px solid %2; border-radius:13px; color:%3; font-size:12px; padding:0; }"
                                "QPushButton:hover { background:%4; }")
                                .arg(btnBg, btnBorder, textMuted, hoverBg));
    connect(closeBtn, &QPushButton::clicked, this, &MainWindow::closeSidebar);
    shLay->addWidget(sTitle);
    shLay->addStretch();
    shLay->addWidget(closeBtn);
    slay->addWidget(sHeader);

    // Menu items
    auto makeItem = [&](const QString &icon, const QString &label) -> QPushButton *
    {
        QPushButton *btn = new QPushButton(QString("  %1  %2").arg(icon, label));
        btn->setFixedHeight(48);
        btn->setStyleSheet(QString(
                               "QPushButton { background: transparent; border: none; border-bottom: 1px solid %1;"
                               " color: %2; font-size: 13px; text-align: left; padding-left: 8px; border-radius: 0; }"
                               "QPushButton:hover { background: %3; }"
                               "QPushButton:pressed { background: %4; }")
                               .arg(sidebarBorder, textPrimary, hoverBg, btnBg));
        return btn;
    };

    QPushButton *itemSettings = makeItem("⚙", "Settings");
    QPushButton *itemHowToUse = makeItem("?", "How to Use");
    QPushButton *itemAbout = makeItem("ℹ", "About");

    connect(itemSettings, &QPushButton::clicked, this, [this]
            { closeSidebar(); showSettingsModal(); });
    connect(itemHowToUse, &QPushButton::clicked, this, [this]
            { closeSidebar(); showHowToUseModal(); });
    connect(itemAbout, &QPushButton::clicked, this, [this]
            { closeSidebar(); showAboutModal(); });

    slay->addWidget(itemSettings);
    slay->addWidget(itemHowToUse);
    slay->addWidget(itemAbout);
    slay->addStretch();

    m_sidebar->show();
    m_sidebar->raise();

    // Animate slide-in
    QPropertyAnimation *anim = new QPropertyAnimation(m_sidebar, "geometry");
    anim->setDuration(320);
    anim->setStartValue(QRect(-220, 0, 220, central->height()));
    anim->setEndValue(QRect(0, 0, 220, central->height()));
    anim->setEasingCurve(QEasingCurve::OutCubic);
    anim->start(QAbstractAnimation::DeleteWhenStopped);

    // Close on overlay click
    struct SidebarFilter : public QObject
    {
        MainWindow *mw;
        QWidget *overlay;
        QWidget *sidebar;
        SidebarFilter(MainWindow *m, QWidget *o, QWidget *s)
            : QObject(o), mw(m), overlay(o), sidebar(s) {}
        bool eventFilter(QObject *watched, QEvent *e) override
        {
            if (e->type() == QEvent::MouseButtonPress && watched == overlay)
            {
                QMouseEvent *me = static_cast<QMouseEvent *>(e);
                if (!sidebar->geometry().contains(me->pos()))
                    mw->closeSidebar();
                return true;
            }
            if (e->type() == QEvent::Resize && watched == overlay->parentWidget())
            {
                overlay->setGeometry(overlay->parentWidget()->rect());
                sidebar->setFixedHeight(overlay->height());
                return false;
            }
            return false;
        }
    };
    SidebarFilter *sf = new SidebarFilter(this, m_sidebarOverlay, m_sidebar);
    central->installEventFilter(sf);
    m_sidebarOverlay->installEventFilter(sf);
}

void MainWindow::closeSidebar()
{
    if (!m_sidebarOpen || !m_sidebar)
        return;
    QWidget *sb = m_sidebar;
    QWidget *ov = m_sidebarOverlay;
    QPropertyAnimation *anim = new QPropertyAnimation(sb, "geometry");
    anim->setDuration(150);
    anim->setStartValue(sb->geometry());
    anim->setEndValue(QRect(-220, 0, 220, sb->height()));
    anim->setEasingCurve(QEasingCurve::InCubic);
    connect(anim, &QPropertyAnimation::finished, this, [ov, this]
            {
                if (ov) ov->deleteLater();
                m_sidebar = nullptr;
                m_sidebarOverlay = nullptr;
                m_sidebarOpen = false; });
    anim->start(QAbstractAnimation::DeleteWhenStopped);
}

void MainWindow::showSettingsModal()
{
    QWidget *central = this->centralWidget();
    QString modalBg = Theme::primaryBg();
    QString modalBorder = Theme::borderGroup();
    QString textPrimary = Theme::textPrimary();
    QString textMuted = Theme::textMuted();

    QWidget *overlay = new QWidget(central);
    overlay->setGeometry(central->rect());
    overlay->setStyleSheet("background: rgba(0,0,0,160);");
    overlay->show();
    overlay->raise();

    QWidget *card = new QWidget(overlay);
    card->setObjectName("settingsCard");
    card->setFixedWidth(380);
    card->setStyleSheet(QString(
                            "QWidget#settingsCard { background:%1; border:1px solid %2; border-radius:10px; }")
                            .arg(modalBg, modalBorder));

    QVBoxLayout *lay = new QVBoxLayout(card);
    lay->setContentsMargins(28, 24, 28, 24);
    lay->setSpacing(14);

    QLabel *title = new QLabel("Settings");
    title->setStyleSheet(QString("font-size:16px;font-weight:bold;color:%1;background:transparent;").arg(textPrimary));
    lay->addWidget(title);

    const bool solverRunning = worker && worker->isRunning();

    QCheckBox *chkSmart = new QCheckBox("Use smarter karn");
    chkSmart->setChecked(m_smartKarn);
    chkSmart->setEnabled(!solverRunning);
    chkSmart->setToolTip("When 'Karn output' is on, use cubeshape-aware karnify.\n"
                         "i.e. don't karnify less obvious karns, like \"T\", when out of CS.");
    chkSmart->setStyleSheet(QString("background:transparent;font-size:13px;color:%1;").arg(textPrimary));
    connect(chkSmart, &QCheckBox::toggled, this, [this](bool checked)
            {
        m_smartKarn = checked;
        if (!m_rawLines.isEmpty()) {
            // Rebuild karn cache with new mode.
            // m_rawLines[i] is the injected raw display line for solutions, or the plain
            // status text for non-solutions. We need the CLEAN line for karnification;
            // those are in m_solutionLinesForRating (parallel to m_solutionLines).
            m_karnLines.clear();
            m_karnSolutionLines.clear();
            int solIdx = 0;
            for (int i = 0; i < m_rawLines.size(); i++) {
                bool isSol = m_rawLines[i].contains('[') && m_rawLines[i].contains(']');
                QString karnLine = m_rawLines[i]; // default for non-solutions
                if (isSol && solIdx < m_solutionLinesForRating.size()) {
                    // Karnify from the clean (uninjected) line, then re-inject the stored indicator
                    QString cleanLine = m_solutionLinesForRating[solIdx];
                    karnLine = convertLine(cleanLine);
                    if (solIdx < m_sliceIndicators.size())
                        karnLine = injectSliceIndicatorDisplay(karnLine, m_sliceIndicators[solIdx]);
                    m_karnSolutionLines.append(karnLine);
                    solIdx++;
                }
                m_karnLines.append(karnLine);
            }
            if (chkKarnotation->isChecked()) {
                if (m_tableVisible) rebuildTable();
                else if (m_ratingsValid && m_cubeshapeWasActive) onRankErgoToggled(true);
                else rebuildTerminalView();
            }
        } });
    lay->addWidget(chkSmart);

    // ── Abid's Notation ───────────────────────────────────────────────────────
    QCheckBox *chkAbid = new QCheckBox("Abid's notation");
    chkAbid->setChecked(m_abidNotation && !OutputConverter::s_abidFontFamily.isEmpty());
    chkAbid->setEnabled(!OutputConverter::s_abidFontFamily.isEmpty() && !solverRunning);
    chkAbid->setToolTip("Display negative numbers as barred digits\n"
                        "using the Kompact font for display.");
    if (OutputConverter::s_abidFontFamily.isEmpty())
        chkAbid->setToolTip(chkAbid->toolTip() +
                            "\n\n⚠ kompact-font.ttf not found — visual effect unavailable.");
    chkAbid->setStyleSheet(QString("background:transparent;font-size:13px;color:%1;").arg(textPrimary));
    connect(chkAbid, &QCheckBox::toggled, this, [this](bool checked)
            {
        m_abidNotation = checked;
        if (!m_rawLines.isEmpty()) {
            if (m_tableVisible) rebuildTable();
            else if (m_ratingsValid && m_cubeshapeWasActive) onRankErgoToggled(true);
            else rebuildTerminalView();
        } });
    lay->addWidget(chkAbid);

    QCheckBox *chkIgnoreTrans = new QCheckBox("Ignore move equivalences");
    chkIgnoreTrans->setEnabled(!solverRunning);
    chkIgnoreTrans->setToolTip("REALLY generate all possible algs -\n"
                               "with all the y2 possibilities and things.\n"
                               "Only useful if you don't anticipate a lot of algs initially.\n"
                               "(my experience is that 8 slicers are a struggle, 9 slicers are impossible)");
    chkIgnoreTrans->setChecked(m_ignoreTrans);
    chkIgnoreTrans->setStyleSheet(QString("background:transparent;font-size:13px;color:%1;").arg(textPrimary));
    chkIgnoreTransSetting = chkIgnoreTrans;
    connect(chkIgnoreTrans, &QObject::destroyed, this, [this]()
            { chkIgnoreTransSetting = nullptr; });
    connect(chkIgnoreTrans, &QCheckBox::toggled, this, [this](bool checked)
            {
        m_ignoreTrans = checked;
        updateCommand(); });
    lay->addWidget(chkIgnoreTrans);

    QCheckBox *chkDebug = new QCheckBox("Debug output");
    chkDebug->setEnabled(!solverRunning);
    chkDebug->setToolTip("Show [DEBUG] lines in the terminal showing internal states:\n"
                         "solver arguments, injected position, and table view variables");
    chkDebug->setChecked(m_debugOutput);
    chkDebug->setStyleSheet(QString("background:transparent;font-size:13px;color:%1;").arg(textPrimary));
    connect(chkDebug, &QCheckBox::toggled, this, [this](bool checked)
            { m_debugOutput = checked; });
    lay->addWidget(chkDebug);

    card->show();
    card->adjustSize();

    auto center = [overlay, card]()
    {
        overlay->setGeometry(overlay->parentWidget()->rect());
        card->move((overlay->width() - card->width()) / 2, (overlay->height() - card->height()) / 2);
    };
    center();
    card->raise();

    struct F : public QObject
    {
        QWidget *overlay;
        QWidget *card;
        std::function<void()> fn;
        F(QWidget *o, QWidget *c, std::function<void()> f) : QObject(o), overlay(o), card(c), fn(f) {}
        bool eventFilter(QObject *w, QEvent *e) override
        {
            if (e->type() == QEvent::Resize && w == overlay->parentWidget())
            {
                fn();
                return false;
            }
            if (e->type() == QEvent::MouseButtonPress && w == overlay)
            {
                if (!card->geometry().contains(static_cast<QMouseEvent *>(e)->pos()))
                    overlay->deleteLater();
                return true;
            }
            return false;
        }
    };
    F *f = new F(overlay, card, center);
    central->installEventFilter(f);
    overlay->installEventFilter(f);
}

// -------------------------------------------------------
// showFavoritesModal
// -------------------------------------------------------
void MainWindow::showFavoritesModal()
{
    QWidget *central = this->centralWidget();
    QString modalBg = Theme::primaryBg();
    QString modalBorder = Theme::borderGroup();
    QString textPrimary = Theme::textPrimary();
    QString textMuted = Theme::textMuted();
    QString textSol = Theme::textSolution();
    QString scrollBg = Theme::scrollbarBg();
    QString scrollHandle = Theme::scrollbarHandle();
    QString hoverColor = Theme::hoverBg();

    QWidget *overlay = new QWidget(central);
    overlay->setGeometry(central->rect());
    overlay->setStyleSheet("background: rgba(0,0,0,160);");
    overlay->show();
    overlay->raise();

    int cardW = qMin(640, central->width() - 60);
    int cardH = qMin(560, central->height() - 60);

    QWidget *card = new QWidget(overlay);
    card->setObjectName("favoritesCard");
    card->setFixedSize(cardW, cardH);
    card->setStyleSheet(QString(
                            "QWidget#favoritesCard { background:%1; border:1px solid %2; border-radius:10px; }")
                            .arg(modalBg, modalBorder));

    QVBoxLayout *cardLay = new QVBoxLayout(card);
    cardLay->setContentsMargins(24, 20, 24, 20);
    cardLay->setSpacing(12);

    QLabel *titleLbl = new QLabel("Favorites");
    titleLbl->setStyleSheet(QString("font-size:16px;font-weight:bold;color:%1;background:transparent;").arg(textPrimary));
    cardLay->addWidget(titleLbl);

    QScrollArea *scroll = new QScrollArea();
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setStyleSheet(QString(
                              "QScrollArea { background: transparent; border: none; }"
                              "QScrollBar:vertical { background:%1; width:6px; border-radius:3px; }"
                              "QScrollBar::handle:vertical { background:%2; border-radius:3px; min-height:20px; }"
                              "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height:0; }")
                              .arg(scrollBg, scrollHandle));
    cardLay->addWidget(scroll, 1);

    QWidget *scrollContent = new QWidget();
    scrollContent->setStyleSheet("background: transparent;");
    QVBoxLayout *binsLay = new QVBoxLayout(scrollContent);
    binsLay->setContentsMargins(0, 0, 6, 0);
    binsLay->setSpacing(10);
    binsLay->addStretch(1);
    scroll->setWidget(scrollContent);

    QString binBg = QString("rgba(255,255,255,12)");
    QString binBorder = modalBorder;

    // Shared button stylesheet snippet for title-row icon buttons
    auto iconBtnStyle = [&](const QString &normalColor, const QString &hoverC)
    {
        return QString(
                   "QPushButton { background:transparent; border:none; color:%1; font-size:13px; padding:0; }"
                   "QPushButton:hover { color:%2; }")
            .arg(normalColor, hoverC);
    };

    QLabel *emptyLabel = new QLabel("No favorites yet.\nRight-click an alg in terminal or table view to add one.");
    emptyLabel->setAlignment(Qt::AlignCenter);
    emptyLabel->setStyleSheet(QString("color:%1;font-size:12px;background:transparent;").arg(textMuted));
    emptyLabel->setWordWrap(true);
    binsLay->insertWidget(0, emptyLabel);

    auto addBinWidget = [=, this](const QString &binKey)
    {
        const QStringList algs = m_favorites.value(binKey);
        const QString displayName = m_favNames.value(binKey, binKey);

        // Per-bin font: if any alg contains Abid PUA chars, use the Abid font for all
        const bool binNeedsAbid = !OutputConverter::s_abidFontFamily.isEmpty() && [&]()
        {
            for (const QString &a : algs)
                for (QChar c : a)
                    if (c.unicode() >= 0xe000)
                        return true;
            return false;
        }();
        QFont binAlgFont;
        binAlgFont.setFamily(binNeedsAbid ? OutputConverter::s_abidFontFamily : QString("monospace"));
        // ASCII matches the global 13px font; the Kompact glyphs render visually
        // smaller, so enlarge them — mirroring the terminal's +2pt for Abid.
        binAlgFont.setPixelSize(binNeedsAbid ? 16 : 13);
        const int binAlgLineH = QFontMetrics(binAlgFont).lineSpacing() + 1;
        const bool hasCustomName = m_favNames.contains(binKey);

        QFrame *binFrame = new QFrame();
        binFrame->setObjectName("binFrame");
        binFrame->setStyleSheet(QString(
                                    "QFrame#binFrame { background:%1; border:1px solid %2; border-radius:8px; }"
                                    "QFrame#binFrame * { background:transparent; }")
                                    .arg(binBg, binBorder));

        QVBoxLayout *binLay = new QVBoxLayout(binFrame);
        binLay->setContentsMargins(12, 10, 12, 10);
        binLay->setSpacing(6);

        // ── Title row ──────────────────────────────────────────────────────────
        QHBoxLayout *titleRow = new QHBoxLayout();
        titleRow->setSpacing(4);

        QLabel *keyLabel = new QLabel(displayName);
        keyLabel->setWordWrap(true);
        keyLabel->setToolTip(hasCustomName
                                 ? QString("Apply the configurations from this solve and clear terminal.\n\nConfig: %1").arg(binKey)
                                 : "Apply the configurations from this solve and clear terminal.");
        keyLabel->setCursor(Qt::PointingHandCursor);
        keyLabel->setStyleSheet(QString(
                                    "font-size:12px;font-weight:bold;color:%1;%2")
                                    .arg(textPrimary, hasCustomName ? "" : "font-family:monospace;"));

        // Rename button (✏)
        QPushButton *renameBinBtn = new QPushButton("✏");
        renameBinBtn->setFixedSize(20, 20);
        renameBinBtn->setToolTip("Rename bin");
        renameBinBtn->setCursor(Qt::PointingHandCursor);
        renameBinBtn->setStyleSheet(iconBtnStyle(textMuted, textPrimary));

        // Copy-all button (⧉)
        QPushButton *copyBinBtn = new QPushButton("⧉");
        copyBinBtn->setFixedSize(20, 20);
        copyBinBtn->setToolTip("Copy all algs");
        copyBinBtn->setCursor(Qt::PointingHandCursor);
        copyBinBtn->setStyleSheet(iconBtnStyle(textMuted, textPrimary));

        // Delete bin button (🗑, red)
        QPushButton *delBinBtn = new QPushButton("🗑");
        delBinBtn->setFixedSize(20, 20);
        delBinBtn->setToolTip("Delete this bin");
        delBinBtn->setCursor(Qt::PointingHandCursor);
        delBinBtn->setStyleSheet(
            "QPushButton { background:transparent; border:none; color:#bb3333; font-size:13px; padding:0; }"
            "QPushButton:hover { color:#ff4444; }");

        titleRow->addWidget(keyLabel, 1);
        titleRow->addWidget(renameBinBtn);
        titleRow->addWidget(copyBinBtn);
        titleRow->addWidget(delBinBtn);
        binLay->addLayout(titleRow);

        // Separator
        QFrame *sep = new QFrame();
        sep->setFrameShape(QFrame::HLine);
        sep->setStyleSheet(QString("background:%1;max-height:1px;border:none;").arg(binBorder));
        binLay->addWidget(sep);

        // ── Algs: wall-of-text QPlainTextEdit + parallel delete buttons ────────
        if (!algs.isEmpty())
        {
            QHBoxLayout *algsRow = new QHBoxLayout();
            algsRow->setSpacing(4);
            algsRow->setContentsMargins(0, 0, 0, 0);

            QPlainTextEdit *algsEdit = new QPlainTextEdit(algs.join('\n'));
            algsEdit->setReadOnly(true);
            algsEdit->setProperty("binAlgs", true); // marks it for ASCII-on-copy handling
            algsEdit->setFont(binAlgFont);
            algsEdit->setFrameShape(QFrame::NoFrame);
            algsEdit->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
            algsEdit->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
            algsEdit->setContextMenuPolicy(Qt::NoContextMenu);
            algsEdit->setWordWrapMode(QTextOption::NoWrap);
            algsEdit->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
            // font-size must be set in the QSS too: a stylesheet that sets
            // font-family resets unspecified font props (size) to the app default,
            // overriding setFont(binAlgFont). Keep it in sync with binAlgFont.
            algsEdit->setStyleSheet(QString(
                                        "QPlainTextEdit { background:transparent; color:%1; border:none; padding:0; "
                                        "selection-background-color:%2; font-family:%3; font-size:%4px; }")
                                        .arg(textSol, hoverColor,
                                             binNeedsAbid ? OutputConverter::s_abidFontFamily : QString("monospace"))
                                        .arg(binNeedsAbid ? 16 : 13));
            algsEdit->setFixedHeight(algs.size() * binAlgLineH + 6);

            QVBoxLayout *delLay = new QVBoxLayout();
            delLay->setSpacing(0);
            delLay->setContentsMargins(0, 3, 0, 3);

            for (const QString &alg : algs)
            {
                QPushButton *delAlg = new QPushButton("✕");
                delAlg->setFixedSize(14, binAlgLineH);
                delAlg->setCursor(Qt::PointingHandCursor);
                delAlg->setToolTip("Remove from bin");
                delAlg->setStyleSheet(iconBtnStyle(textMuted, "#ff6666"));
                delLay->addWidget(delAlg);

                connect(delAlg, &QPushButton::clicked, this, [=, this]()
                        {
                    m_favorites[binKey].removeAll(alg);
                    if (m_favorites[binKey].isEmpty())
                        m_favorites.remove(binKey);
                    saveSettings();
                    overlay->deleteLater();
                    showFavoritesModal(); });
            }
            delLay->addStretch();

            algsRow->addWidget(algsEdit, 1);
            algsRow->addLayout(delLay);
            binLay->addLayout(algsRow);
        }

        binsLay->insertWidget(binsLay->count() - 1, binFrame);

        // ── Connections ────────────────────────────────────────────────────────
        connect(copyBinBtn, &QPushButton::clicked, this, [algs]()
                {
            QStringList clean;
            for (const QString &a : algs) {
                int lb = a.lastIndexOf('[');
                clean << OutputConverter::deabidify(lb > 0 ? a.left(lb).trimmed() : a.trimmed());
            }
            QApplication::clipboard()->setText(clean.join('\n')); });

        connect(delBinBtn, &QPushButton::clicked, this, [=, this]()
                {
            m_favorites.remove(binKey);
            m_favNames.remove(binKey);
            saveSettings();
            overlay->deleteLater();
            showFavoritesModal(); });

        connect(renameBinBtn, &QPushButton::clicked, this, [=, this]()
                {
            QDialog dlg(this);
            dlg.setWindowTitle("Rename Bin");
            dlg.setMinimumWidth(420);
            dlg.setStyleSheet(QString(
                "QDialog { background:%1; color:%2; }"
                "QLabel { color:%2; background:transparent; }"
                "QPlainTextEdit { background:%3; color:%2; border:1px solid %4; border-radius:4px; padding:4px; }"
                "QPushButton { background:%3; color:%2; border:1px solid %4; border-radius:4px; padding:4px 12px; }"
                "QPushButton:hover { background:%5; }")
                .arg(modalBg, textPrimary, Theme::tertiaryBg(), modalBorder, hoverColor));
            QVBoxLayout *lay = new QVBoxLayout(&dlg);
            lay->setSpacing(10);
            QLabel *hint = new QLabel("Custom display name (leave blank to use the config key):");
            hint->setWordWrap(true);
            QPlainTextEdit *edit = new QPlainTextEdit();
            edit->setPlainText(m_favNames.value(binKey, ""));
            edit->setFixedHeight(80);
            QHBoxLayout *btnLay = new QHBoxLayout();
            QPushButton *ok = new QPushButton("Save");
            QPushButton *cancel = new QPushButton("Cancel");
            btnLay->addStretch();
            btnLay->addWidget(cancel);
            btnLay->addWidget(ok);
            lay->addWidget(hint);
            lay->addWidget(edit);
            lay->addLayout(btnLay);
            connect(ok, &QPushButton::clicked, &dlg, &QDialog::accept);
            connect(cancel, &QPushButton::clicked, &dlg, &QDialog::reject);
            if (dlg.exec() == QDialog::Accepted) {
                QString newName = edit->toPlainText().trimmed();
                if (newName.isEmpty())
                    m_favNames.remove(binKey);
                else
                    m_favNames[binKey] = newName;
                saveSettings();
                overlay->deleteLater();
                showFavoritesModal();
            } });

        // Click title label to apply config
        struct TitleClick : public QObject
        {
            QLabel *lbl;
            std::function<void()> fn;
            TitleClick(QLabel *l, std::function<void()> f) : QObject(l), lbl(l), fn(f) {}
            bool eventFilter(QObject *w, QEvent *e) override
            {
                if (e->type() == QEvent::MouseButtonPress && w == lbl)
                {
                    fn();
                    return true;
                }
                return false;
            }
        };
        auto *tc = new TitleClick(keyLabel, [=, this]()
                                  {
            overlay->deleteLater();
            applyRunConfig(binKey); });
        keyLabel->installEventFilter(tc);
    };

    if (m_favorites.isEmpty())
    {
        emptyLabel->setVisible(true);
    }
    else
    {
        emptyLabel->setVisible(false);
        for (auto it = m_favorites.constBegin(); it != m_favorites.constEnd(); ++it)
            addBinWidget(it.key());
    }

    card->show();
    card->adjustSize();

    auto center = [overlay, card, cardW, cardH]()
    {
        overlay->setGeometry(overlay->parentWidget()->rect());
        card->move((overlay->width() - cardW) / 2, (overlay->height() - cardH) / 2);
    };
    center();
    card->raise();

    struct F : public QObject
    {
        QWidget *overlay;
        QWidget *card;
        std::function<void()> fn;
        F(QWidget *o, QWidget *c, std::function<void()> f) : QObject(o), overlay(o), card(c), fn(f) {}
        bool eventFilter(QObject *w, QEvent *e) override
        {
            if (e->type() == QEvent::Resize && w == overlay->parentWidget())
            {
                fn();
                return false;
            }
            if (e->type() == QEvent::MouseButtonPress && w == overlay)
            {
                if (!card->geometry().contains(static_cast<QMouseEvent *>(e)->pos()))
                    overlay->deleteLater();
                return true;
            }
            return false;
        }
    };
    F *f = new F(overlay, card, center);
    central->installEventFilter(f);
    overlay->installEventFilter(f);
}

void MainWindow::showHowToUseModal()
{
    QWidget *central = this->centralWidget();
    QString modalBg = Theme::primaryBg();
    QString modalBorder = Theme::borderGroup();
    QString textPrimary = Theme::textPrimary();
    QString textBody = Theme::textSecondary();
    QString textCyan = Theme::textCyan();
    QString scrollBg = Theme::scrollbarBg();
    QString scrollHandle = Theme::scrollbarHandle();

    QWidget *overlay = new QWidget(central);
    overlay->setGeometry(central->rect());
    overlay->setStyleSheet("background: rgba(0,0,0,160);");
    overlay->show();
    overlay->raise();

    QWidget *card = new QWidget(overlay);
    card->setObjectName("howToCard");
    int cardW = qMin(560, central->width() - 60);
    int cardH = qMin(520, central->height() - 80);
    card->setFixedSize(cardW, cardH);
    card->setStyleSheet(QString(
                            "QWidget#howToCard { background:%1; border:1px solid %2; border-radius:10px; }")
                            .arg(modalBg, modalBorder));

    QVBoxLayout *lay = new QVBoxLayout(card);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);

    // Title bar
    QWidget *titleBar = new QWidget();
    titleBar->setFixedHeight(48);
    titleBar->setStyleSheet(QString(
                                "QWidget { background: %1; border-bottom: 1px solid %2;"
                                " border-top-left-radius: 10px; border-top-right-radius: 10px; border-bottom-left-radius:0; border-bottom-right-radius:0; }")
                                .arg(modalBg, modalBorder));
    QHBoxLayout *tbLay = new QHBoxLayout(titleBar);
    tbLay->setContentsMargins(20, 0, 16, 0);
    QLabel *titleLbl = new QLabel("How to Use");
    titleLbl->setStyleSheet(QString("font-size:15px;font-weight:bold;color:%1;background:transparent;").arg(textPrimary));
    tbLay->addWidget(titleLbl);
    tbLay->addStretch();
    lay->addWidget(titleBar);

    // Scrollable content
    QScrollArea *sa = new QScrollArea();
    sa->setWidgetResizable(true);
    sa->setFrameShape(QFrame::NoFrame);
    sa->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    sa->setStyleSheet(QString(
                          "QScrollArea { background: transparent; }"
                          "QScrollBar:vertical { background: %1; width: 6px; border-radius: 3px; }"
                          "QScrollBar::handle:vertical { background: %2; border-radius: 3px; min-height: 20px; }"
                          "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height:0; border:0; }")
                          .arg(scrollBg, scrollHandle));

    QLabel *body = new QLabel();
    body->setWordWrap(true);
    body->setOpenExternalLinks(false);
    body->setTextFormat(Qt::RichText);
    body->setContentsMargins(20, 16, 20, 16);
    body->setStyleSheet(QString("background: transparent; color: %1; font-size: 12px; line-height: 1.7;").arg(textBody));
    body->setText(QString(
        "<b style='color:%1;font-size:13px;'>Keyboard shortcuts:</b><br>"
        "• <b style='color:%2;'>Z</b> = Undo &nbsp; <b style='color:%2;'>Y</b> = Redo<br>"
        "• <b style='color:%2;'>Esc</b> = Reset the cube to solved<br>"
        "• <b style='color:%2;'>Ctrl + Enter</b> = Start or stop the solver<br>"
        "• <b style='color:%2;'>Ctrl + Z</b> = Undo state change &nbsp; <b style='color:%2;'>Ctrl + Y</b> = Redo state change<br>"
        "• <b style='color:%2;'>Ctrl + =</b> = Zoom in &nbsp; <b style='color:%2;'>Ctrl + -</b> = Zoom out<br>"
        "• <b style='color:%2;'>Ctrl + 0</b> = Reset zoom level<br><br>"
        "Click on two pieces to <b>swap</b> them. Or use the below shortcuts (identical to cstimer):<br>"
        "• <b style='color:%2;'>J</b> = U, only by one piece &nbsp; <b style='color:%2;'>F</b> = U', only by one piece <br>"
        "• <b style='color:%2;'>S</b> = D, only by one piece &nbsp; <b style='color:%2;'>L</b> = D', only by one piece <br>"
        "• <b style='color:%2;'>I</b> or <b style='color:%2;'>K</b> = Slice<br>"
        "• <b style='color:%2;'>H</b> = U, by two pieces &nbsp; <b style='color:%2;'>G</b> = U', by two pieces<br>"
        "• <b style='color:%2;'>W</b> = D, by two pieces &nbsp; <b style='color:%2;'>O</b> = D', by two pieces<br><br>"

        "<b style='color:%1;font-size:13px;'>Scramble / Alg Input</b><br>"
        "Type some moves and hit <b>Apply</b>. Karn will be parsed correctly.<br>"
        "Use the mode button (to the left of the input) to switch between three modes:<br>"
        "• <b>Scram</b>: applies moves forward as a scramble<br>"
        "• <b>Alg</b>: inverts before applying, useful for testing algs<br>"
        "• <b>Pos</b>: interprets the input as a string of the raw state (e.g. A1B2C4D38E6F7G5H)<br>"
        "For the first two modes, use <b style='color:%2;'>Enter</b> to apply the moves from the current state, or <b style='color:%2;'>Shift + Enter</b> to first reset the cube to solved state before applying.<br><br>"

        "<b style='color:%1;font-size:13px;'>Favorites</b><br>"
        "Algs can be saved to bins for later reference. "
        "Right-click a generated alg to:<br>"
        "• <b>⧉ Copy alg</b> — copies the alg itself<br>"
        "• <b>♥ Add to Favorites Bin</b> — saves the alg to a bin<br><br>"
        "Bins are identified by the <b>configurations</b> of the solve, so algs from the same setup always land in the same bin regardless of when they were added.<br>"
        "Click the <b>♥</b> button (visible in the terminal area) to open the Favorites Bin, where you can:<br>"
        "• Click a <b>bin title</b> to re-apply that configuration and clear the terminal.<br>"
        "• Use <b>✏</b> to rename a bin<br>"
        "• <b>⧉</b> to copy all its algs<br>"
        "• <b>🗑</b> to delete the bin entirely<br>"
        "• Click <b>✕</b> next to any alg to remove it<br>"
        "Favorited algs are stored on your device, unless you delete them.<br><br>"

        "<b style='color:%1;font-size:13px;'>Options</b><br>"
        "Hover over any option to read its description. Quick reference:<br>"
        "• <b>Metric</b>: how move length is counted — <b>Slice</b> (only slices), <b>Move</b> (layer turns too), or <b>Angle</b>.<br>"
        "• <b>All optimal</b>: find every shortest solution, not just the first one found.<br>"
        "• <b>+suboptimal</b>: also return solutions up to N moves longer than optimal.<br>"
        "• <b>Specific depths</b>: search only the listed move counts (comma-separated, e.g. \"8,9\").<br>"
        "• <b>Generator alg</b>: output algs will set up the case from solved instead of solving it.<br>"
        "• <b>2 Gen / Pseudo 2 Gen</b>: restrict moves to top-layer turns and slices (or a pseudo variant).<br>"
        "• <b>Stay in cubeshape</b>: restrict to algs that keep the puzzle in cubeshape throughout.<br>"
        "• <b>Karn output</b>: display solutions in karn instead of WCA notation.<br>"
        "• <b>Lock layer angle on pre-ABF</b>: constrain the pre-ABF move to ±1 or 0 on either/both layers.<br>"
        "• <b>Normalize ABF</b>: simplify ABF moves in the output (e.g. 3-1 → 0-1, 43 → 10).<br>"
        "• <b>Max top / bottom / total turns</b>: cap how large layer turns can be.<br><br>"

        "<b style='color:%1;font-size:13px;'>Settings</b><br>"
        "• <b>Use smarter karn</b>: don't karnify things like T when the alg goes out of CS.<br>"
        "• <b>Abid's notation</b>: use barred numbers for negatives. This is just a display setting.<br>"
        "• <b>Ignore move equivalences</b>: generate all possible algs, even with y2 algs.<br>"
        "• <b>Debug output</b>: outputs internal solver states.<br><br>"

        "<b style='color:%1;font-size:13px;'>Output</b><br>"
        "Solutions appear in the terminal as they are found. Once algs are present, several buttons appear in the corner of the terminal area:<br>"
        "• <b>⧉</b> — copy all algs in the terminal to the clipboard.<br>"
        "• <b>♥</b> — see the Favorites section.<br>"
        "• <b>⊞</b> — switch between terminal view and table view.<br>"
        "• <b>⤢</b> — expand the terminal to full screen.<br>"
        "If <b>Stay in cubeshape</b> was active, algs will be roughly sorted by their <b>ergonomics</b>. The numbers are relative and for reference only.<br>")
        .arg(textPrimary, textCyan));

    sa->setWidget(body);
    lay->addWidget(sa, 1);

    card->show();

    auto center = [overlay, card, central]()
    {
        overlay->setGeometry(overlay->parentWidget()->rect());
        int cw = qMin(560, central->width() - 60);
        int ch = qMin(520, central->height() - 80);
        card->setFixedSize(cw, ch);
        card->move((overlay->width() - card->width()) / 2, (overlay->height() - card->height()) / 2);
    };
    center();
    card->raise();

    struct F : public QObject
    {
        QWidget *overlay;
        QWidget *card;
        std::function<void()> fn;
        F(QWidget *o, QWidget *c, std::function<void()> f) : QObject(o), overlay(o), card(c), fn(f) {}
        bool eventFilter(QObject *w, QEvent *e) override
        {
            if (e->type() == QEvent::Resize && w == overlay->parentWidget())
            {
                fn();
                return false;
            }
            if (e->type() == QEvent::MouseButtonPress && w == overlay)
            {
                if (!card->geometry().contains(static_cast<QMouseEvent *>(e)->pos()))
                    overlay->deleteLater();
                return true;
            }
            return false;
        }
    };
    F *f = new F(overlay, card, center);
    central->installEventFilter(f);
    overlay->installEventFilter(f);
}

bool MainWindow::eventFilter(QObject *watched, QEvent *event)
{
    // ── (0) Per-line tooltips for the output box ──────────────────────────────
    // QTextEdit delivers QHelpEvent to its internal viewport, not to itself.
    if (event->type() == QEvent::ToolTip && txtOutput && watched == txtOutput->viewport())
        return true;

    // ── Suppress all native tooltips; we handle them via MouseMove ───────────
    if (event->type() == QEvent::ToolTip)
        return true;

    // ── Pointing hand only over checkbox indicator+text, arrow elsewhere ──────
    if (event->type() == QEvent::MouseMove || event->type() == QEvent::HoverMove)
    {
        if (QCheckBox *cb = qobject_cast<QCheckBox *>(watched))
        {
            QPoint localPos = cb->mapFromGlobal(QCursor::pos());
            QStyleOptionButton opt;
            opt.initFrom(cb);
            QRect indRect = cb->style()->subElementRect(QStyle::SE_CheckBoxIndicator, &opt, cb);
            QFontMetrics fm(cb->font());
            QRect textRect(indRect.right() + 6, 0, fm.horizontalAdvance(cb->text()), cb->height());
            QRect activeRect = indRect.united(textRect);
            if (activeRect.contains(localPos))
            {
                if (cb != chkDepths)
                    cb->setCursor(Qt::PointingHandCursor);
                if (!cb->toolTip().isEmpty())
                    FadingTooltip::arm(cb->toolTip(), QCursor::pos(), m_zoomView);
            }
            else
            {
                cb->setCursor(Qt::ArrowCursor);
                FadingTooltip::dismiss(m_zoomView);
            }
        }
        else if (event->type() == QEvent::HoverMove)
        {
            // HoverMove on a non-checkbox widget — arm to keep tooltip alive while
            // cursor stays within the widget (e.g. a button that uses WA_Hover).
            QWidget *w = qobject_cast<QWidget *>(watched);
            QAbstractButton *btn = qobject_cast<QAbstractButton *>(w);
            if (btn && !btn->toolTip().isEmpty())
            {
                // Re-arm via button geometry so position stays fixed and dismiss is cancelled.
                // Only use the proxy transform when the button is actually inside the proxy
                // widget tree; overlays (e.g. favorites modal) are not, and mapTo would crash.
                QWidget *proxyRoot = m_zoomProxy ? m_zoomProxy->widget() : nullptr;
                bool inProxy = false;
                for (QWidget *p = btn; p && !inProxy; p = p->parentWidget())
                    inProxy = (p == proxyRoot);
                QRect globalRect;
                if (inProxy && proxyRoot)
                {
                    QPoint tl = btn->mapTo(proxyRoot, QPoint(0, 0));
                    QPoint br = btn->mapTo(proxyRoot, QPoint(btn->width(), btn->height()));
                    QPoint viewTL = m_zoomView->mapFromScene(m_zoomProxy->mapToScene(QPointF(tl)));
                    QPoint viewBR = m_zoomView->mapFromScene(m_zoomProxy->mapToScene(QPointF(br)));
                    globalRect = QRect(m_zoomView->viewport()->mapToGlobal(viewTL),
                                       m_zoomView->viewport()->mapToGlobal(viewBR));
                }
                else
                {
                    globalRect = QRect(btn->mapToGlobal(QPoint(0, 0)),
                                       btn->mapToGlobal(QPoint(btn->width(), btn->height())));
                }
                FadingTooltip::armButton(btn->toolTip(), globalRect, m_zoomView);
            }
            else
            {
                QString tip;
                while (w && tip.isEmpty())
                {
                    tip = w->toolTip();
                    w = w->parentWidget();
                }
                if (!tip.isEmpty())
                    FadingTooltip::arm(tip, QCursor::pos(), m_zoomView);
            }
            // No dismiss on HoverMove — HoverLeave handles leaving the widget.
        }
        else if (event->type() == QEvent::MouseMove)
        {
            // Walk up the widget hierarchy: if any ancestor has a tooltip, show it.
            // This makes radio-row containers and spinboxes work without special-casing.
            QWidget *w = qobject_cast<QWidget *>(watched);
            QString tip;
            while (w && tip.isEmpty())
            {
                tip = w->toolTip();
                w = w->parentWidget();
            }
            if (!tip.isEmpty())
                FadingTooltip::arm(tip, QCursor::pos(), m_zoomView);
            else
                FadingTooltip::dismiss(m_zoomView);
        }
    }

    // ── Tooltip arm/dismiss for hover-only widgets (QPushButton etc.) ─────────
    // Buttons set WA_Hover and receive HoverEnter/HoverLeave instead of MouseMove.
    // HoverEnter fires exactly once per entry — no double-dispatch race with viewport.
    if (event->type() == QEvent::HoverEnter)
    {
        QWidget *w = qobject_cast<QWidget *>(watched);
        if (w && !qobject_cast<QCheckBox *>(w))
        {
            if (qobject_cast<QAbstractButton *>(w) && !w->toolTip().isEmpty())
            {
                // Compute the button's true screen rect through the zoom transform:
                // widget → proxy-root → scene → viewport → global.
                // For widgets outside the proxy (e.g. favorites modal overlay), fall
                // back to mapToGlobal directly to avoid a null-parent crash in mapTo.
                QWidget *proxyRoot = m_zoomProxy ? m_zoomProxy->widget() : nullptr;
                bool inProxy = false;
                for (QWidget *p = w; p && !inProxy; p = p->parentWidget())
                    inProxy = (p == proxyRoot);
                QRect globalRect;
                if (inProxy && proxyRoot)
                {
                    QPoint tl = w->mapTo(proxyRoot, QPoint(0, 0));
                    QPoint br = w->mapTo(proxyRoot, QPoint(w->width(), w->height()));
                    QPoint viewTL = m_zoomView->mapFromScene(m_zoomProxy->mapToScene(QPointF(tl)));
                    QPoint viewBR = m_zoomView->mapFromScene(m_zoomProxy->mapToScene(QPointF(br)));
                    globalRect = QRect(m_zoomView->viewport()->mapToGlobal(viewTL),
                                       m_zoomView->viewport()->mapToGlobal(viewBR));
                }
                else
                {
                    globalRect = QRect(w->mapToGlobal(QPoint(0, 0)),
                                       w->mapToGlobal(QPoint(w->width(), w->height())));
                }
                FadingTooltip::armButton(w->toolTip(), globalRect, m_zoomView);
            }
            else
            {
                QString tip;
                while (w && tip.isEmpty())
                {
                    tip = w->toolTip();
                    w = w->parentWidget();
                }
                if (!tip.isEmpty())
                    FadingTooltip::arm(tip, QCursor::pos(), m_zoomView);
            }
        }
    }
    if (event->type() == QEvent::HoverLeave)
    {
        if (!qobject_cast<QCheckBox *>(watched))
        {
            QWidget *w = qobject_cast<QWidget *>(watched);
            QString tip;
            while (w && tip.isEmpty())
            {
                tip = w->toolTip();
                w = w->parentWidget();
            }
            // Only dismiss when leaving a widget that actually owned the tooltip,
            // so viewport HoverLeave (no tooltip) never spuriously dismisses.
            if (!tip.isEmpty())
                FadingTooltip::dismiss(m_zoomView);
        }
    }

    // ── Right-click copy on table-cell labels (always copies clean notation) ──
    if (event->type() == QEvent::ContextMenu)
    {
        if (QLabel *lbl = qobject_cast<QLabel *>(watched))
        {
            QVariant v = lbl->property("cleanAlg");
            if (v.isValid())
            {
                QMenu menu;
                QAction *copyAct = menu.addAction("Copy");
                QAction *chosen = menu.exec(
                    static_cast<QContextMenuEvent *>(event)->globalPos());
                if (chosen == copyAct)
                    QApplication::clipboard()->setText(v.toString());
                return true;
            }
        }
        // Suppress the default QTextEdit context menu on the terminal
        if (txtOutput && (watched == txtOutput || watched == txtOutput->viewport()))
            return true;
    }

    // ── Right-click on terminal / Left or right click on table ────────────────
    if (event->type() == QEvent::MouseButtonPress && txtOutput &&
        watched == txtOutput->viewport())
    {
        QMouseEvent *me = static_cast<QMouseEvent *>(event);
        if (me->button() == Qt::RightButton)
        {
            QTextCursor tc = txtOutput->cursorForPosition(me->pos());
            if (AlgBlockData *data = dynamic_cast<AlgBlockData *>(tc.block().userData()))
                showAlgContextMenu(me->globalPos(), data->rawLine);
            return true;
        }
    }

    if (event->type() == QEvent::MouseButtonPress && m_solutionTable)
    {
        QMouseEvent *me = static_cast<QMouseEvent *>(event);
        if (watched == m_solutionTable->viewport() &&
            (me->button() == Qt::LeftButton || me->button() == Qt::RightButton))
        {
            int row = m_solutionTable->rowAt(me->pos().y());
            if (row >= 0)
            {
                QTableWidgetItem *numItem = m_solutionTable->item(row, 0);
                if (numItem)
                {
                    QString rawLine = numItem->data(Qt::UserRole).toString();
                    if (!rawLine.isEmpty())
                    {
                        showAlgContextMenu(me->globalPos(), rawLine);
                        return true;
                    }
                }
            }
        }
    }

    // ── Enter/Leave on floating buttons — keep visible while hovering ─────────
    // Effects are disabled at full opacity (setOutputBtnsOpacity handles this),
    // so no special effect toggling is needed on hover — QSS :hover works fine
    // through the normal paint path.
    // Qt delivers Enter to the newly-entered widget BEFORE Leave to the old one,
    // so underMouse() is already accurate when any Leave handler runs.
    auto anyBtnHovered = [this]()
    {
        return btnCopyTerminal->underMouse() || btnFavorites->underMouse() || btnTableMode->underMouse() || btnExpand->underMouse();
    };
    if (event->type() == QEvent::Enter)
    {
        if (watched == btnCopyTerminal || watched == btnFavorites || watched == btnTableMode || watched == btnExpand)
        {
            if (m_outputIdleTimer)
                m_outputIdleTimer->stop();
            if (!m_outputBtnsFullOpacity)
                setOutputBtnsOpacity(1.0, 0);
            return false;
        }
    }
    if (event->type() == QEvent::Leave)
    {
        if (watched == btnCopyTerminal || watched == btnFavorites || watched == btnTableMode || watched == btnExpand)
        {
            if (anyBtnHovered())
                return false; // moved to another button
            // All buttons left — resume idle timer if still in the output area,
            // or start fading now if the cursor left the area entirely.
            if (m_outputWrapper && m_outputWrapper->rect().contains(
                                       m_outputWrapper->mapFromGlobal(QCursor::pos())))
            {
                if (m_outputIdleTimer)
                    m_outputIdleTimer->start(1500);
            }
            else
            {
                if (m_outputIdleTimer)
                    m_outputIdleTimer->stop();
                setOutputBtnsOpacity(0.15, 400);
            }
            return false;
        }
        if (watched == m_outputWrapper && !anyBtnHovered())
        {
            if (m_outputIdleTimer)
                m_outputIdleTimer->stop();
            setOutputBtnsOpacity(0.15, 400);
            return false;
        }
    }

    // ── Mouse movement inside output wrapper — reset idle-fade timer ──────────
    // QEvent::Wheel is a distinct event type, so scroll-wheel activity never
    // reaches this branch and never reactivates the faded buttons.
    if (event->type() == QEvent::MouseMove && m_outputWrapper)
    {
        if (QWidget *w = qobject_cast<QWidget *>(watched))
        {
            if (w == m_outputWrapper || m_outputWrapper->isAncestorOf(w))
                onOutputMouseActive();
        }
    }

    if (event->type() == QEvent::Resize && watched == m_outputWrapper)
    {
        int w = m_outputWrapper->width();
        int h = m_outputWrapper->height();
        int margin = 6;
        int bw = 22;
        btnExpand->move(w - margin - bw, margin);
        btnTableMode->move(w - margin - bw * 2 - 4, margin);
        btnFavorites->move(w - margin - bw * 3 - 8, margin);
        btnCopyTerminal->move(w - margin - bw * 4 - 12, margin);
        btnScrollToBottom->move(w - margin - 28 - 16, h - margin - 32 - margin);
        return false;
    }

    // ── Scroll-up on terminal viewport pauses auto-scroll ────────────────────
    if (event->type() == QEvent::Wheel && txtOutput && watched == txtOutput->viewport())
    {
        QWheelEvent *we = static_cast<QWheelEvent *>(event);
        bool scrollingUp = we->angleDelta().y() > 0;
        if (scrollingUp && !m_autoScrollPaused && (worker && worker->isRunning()))
        {
            m_autoScrollPaused = true;
            btnScrollToBottom->setGraphicsEffect(nullptr);
            btnScrollToBottom->setText("⌄");
            btnScrollToBottom->setStyleSheet(""); // revert to QSS default
            int w = m_outputWrapper->width();
            int h = m_outputWrapper->height();
            btnScrollToBottom->move(w - 6 - 28 - 16, h - 6 - 32 - 6);
            btnScrollToBottom->raise();
            btnScrollToBottom->setVisible(true);
        }
        return false; // let the scroll happen normally
    }

    if (event->type() != QEvent::KeyPress)
        return QMainWindow::eventFilter(watched, event);

    // ── Block all keyboard shortcuts while a modal dialog is open ────────────
    // This prevents cube movement, undo/redo, zoom, etc. from firing while the
    // user is focused on e.g. the rename-bin popup's text field.
    if (QApplication::activeModalWidget())
        return QMainWindow::eventFilter(watched, event);

    QKeyEvent *ke = static_cast<QKeyEvent *>(event);

    // ── (1) Ctrl+C — copy only; it must NOT stop the solver (use Ctrl+Enter) ──
    if (ke->key() == Qt::Key_C && (ke->modifiers() & Qt::ControlModifier))
    {
        // Terminal selection in Abid display: copy the clean alg for any solution
        // line touched (the displayed glyphs aren't plain text). Without Abid
        // display the default copy is already correct, so leave it alone.
        if (m_abidNotation && !OutputConverter::s_abidFontFamily.isEmpty() &&
            txtOutput && (txtOutput->hasFocus() || txtOutput->viewport()->hasFocus()))
        {
            if (copyTerminalSelection())
                return true;
        }
        // Favorites-bin alg list: any selection copies the whole touched alg(s) in
        // ASCII (deabidified), mirroring the terminal copy behavior.
        if (QPlainTextEdit *pe = qobject_cast<QPlainTextEdit *>(QApplication::focusWidget()))
        {
            if (pe->property("binAlgs").toBool() && pe->textCursor().hasSelection())
            {
                QTextCursor c = pe->textCursor();
                const int selStart = c.selectionStart(), selEnd = c.selectionEnd();
                QStringList out;
                for (QTextBlock b = pe->document()->findBlock(selStart);
                     b.isValid() && b.position() <= selEnd; b = b.next())
                {
                    if (b.position() == selEnd && selStart != selEnd)
                        break;
                    QString t = b.text();
                    int lb = t.lastIndexOf('[');
                    out << OutputConverter::deabidify(lb > 0 ? t.left(lb).trimmed() : t.trimmed());
                }
                QApplication::clipboard()->setText(out.join('\n'));
                return true;
            }
        }
        // If Ctrl+C fires on a table-cell item while Abid's notation is on,
        // copy the clean (minus-sign) text instead of the PUA glyphs.
        if (m_abidNotation && !OutputConverter::s_abidFontFamily.isEmpty())
        {
            QModelIndex idx = m_solutionTable->currentIndex();
            if (idx.isValid() && idx.column() == 1)
            {
                QVariant v = m_solutionTable->model()->data(idx, Qt::UserRole);
                if (v.isValid())
                {
                    QApplication::clipboard()->setText(v.toString());
                    return true;
                }
            }
        }
        return QMainWindow::eventFilter(watched, event);
    }

    // ── Ctrl+= / Ctrl+- — zoom in / out ─────────────────────────────────────
    if (ke->modifiers() == Qt::ControlModifier)
    {
        if (ke->key() == Qt::Key_Equal || ke->key() == Qt::Key_Plus)
        {
            m_zoomScale = qMin(m_zoomScale + 0.1, 2.0);
            applyZoom();
            return true;
        }
        if (ke->key() == Qt::Key_Minus)
        {
            m_zoomScale = qMax(m_zoomScale - 0.1, 0.5);
            applyZoom();
            return true;
        }
        if (ke->key() == Qt::Key_0)
        {
            m_zoomScale = 1.0;
            applyZoom();
            return true;
        }
    }

    // ── Ctrl+Z / Ctrl+Y — undo / redo (work from any widget, including inputs) ─
    // Blocked while solver is running (controls are disabled).
    if (ke->modifiers() == Qt::ControlModifier && !(worker && worker->isRunning()))
    {
        if (ke->key() == Qt::Key_Z)
        {
            if (!m_undoStack.isEmpty())
                btnUndo->click();
            return true;
        }
        if (ke->key() == Qt::Key_Y)
        {
            if (!m_redoStack.isEmpty())
                btnRedo->click();
            return true;
        }
    }

    // ── Events already targeting cubeWidget: let cubeWidget handle them. ─────
    // (sendEvent below will re-enter here with watched == cubeWidget.)
    if (watched == cubeWidget)
        return QMainWindow::eventFilter(watched, event);

    // ── Esc — dismiss visible tooltip first; cube reset on the next Esc ─────
    if (ke->key() == Qt::Key_Escape && ke->modifiers() == Qt::NoModifier && FadingTooltip::active(m_zoomView))
    {
        FadingTooltip::dismissNow(m_zoomView);
        return true;
    }

    // ── Esc from m_mainInput — reset cube without stealing focus ─────────────
    if ((watched == m_mainInput || QApplication::focusWidget() == m_mainInput) && ke->key() == Qt::Key_Escape && ke->modifiers() == Qt::NoModifier)
    {
        m_redoStack.clear();
        btnUndo->setEnabled(false);
        btnRedo->setEnabled(false);
        QKeyEvent escEv(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
        QApplication::sendEvent(cubeWidget, &escEv);
        return true;
    }

    // ── Enter / Shift+Enter in m_mainInput ───────────────────────────────────
    // Enter        = apply alg/scramble from current cube state
    // Shift+Enter  = apply alg/scramble from solved state (resets first)
    // Blocked while solver is running (Apply is disabled).
    if ((watched == m_mainInput || QApplication::focusWidget() == m_mainInput) && ke->key() == Qt::Key_Return && !(worker && worker->isRunning()))
    {
        if (ke->modifiers() & Qt::ShiftModifier)
        {
            // Shift+Enter: apply from solved state
            m_applyFromSolved = true;
            btnApply->click();
            m_applyFromSolved = false;
        }
        else
        {
            // plain Enter: apply on top of current cube state
            m_applyFromSolved = false;
            btnApply->click();
        }
        return true;
    }

    // ── Text inputs get all remaining keys — never steal from them ────────────
    {
        QWidget *fw = QApplication::focusWidget();
        if (fw == txtCommand || fw == txtDepths || fw == m_mainInput ||
            watched == txtCommand || watched == txtDepths || watched == m_mainInput ||
            m_mainInput->hasFocus() || txtCommand->hasFocus() || txtDepths->hasFocus())
            return QMainWindow::eventFilter(watched, event);
    }

    // ── (2) Route cube shortcuts from any other widget ────────────────────────
    // Blocked while solver is running (cube is disabled).
    if (ke->modifiers() == Qt::NoModifier && !(worker && worker->isRunning()))
    {
        auto sendCube = [this](Qt::Key k)
        {
            QKeyEvent e(QEvent::KeyPress, k, Qt::NoModifier);
            QApplication::sendEvent(cubeWidget, &e);
        };
        bool handled = true;
        switch (ke->key())
        {
        case Qt::Key_I:
        case Qt::Key_K:
        {
            m_sliceCount++;
            m_slicePending.append({cubeWidget->getPositionString()});
            sendCube(static_cast<Qt::Key>(ke->key()));
            m_sliceTimer->start(600);
            break;
        }
        case Qt::Key_J:
            pushUndoState();
            sendCube(Qt::Key_J);
            break;
        case Qt::Key_F:
            pushUndoState();
            sendCube(Qt::Key_F);
            break;
        case Qt::Key_S:
            pushUndoState();
            sendCube(Qt::Key_S);
            break;
        case Qt::Key_L:
            pushUndoState();
            sendCube(Qt::Key_L);
            break;
        case Qt::Key_Escape:
            m_undoStack.clear();
            m_redoStack.clear();
            btnUndo->setEnabled(false);
            btnRedo->setEnabled(false);
            sendCube(Qt::Key_Escape);
            break;
        case Qt::Key_H:
            pushUndoState();
            sendCube(Qt::Key_J);
            sendCube(Qt::Key_J);
            break; // UU
        case Qt::Key_G:
            pushUndoState();
            sendCube(Qt::Key_F);
            sendCube(Qt::Key_F);
            break; // U'U'
        case Qt::Key_O:
            pushUndoState();
            sendCube(Qt::Key_L);
            sendCube(Qt::Key_L);
            break; // D'D'
        case Qt::Key_W:
            pushUndoState();
            sendCube(Qt::Key_S);
            sendCube(Qt::Key_S);
            break; // DD
        default:
            handled = false;
            break;
        }
        if (handled)
            return true; // consume — letter goes to cube, not to any text field
    }
    return QMainWindow::eventFilter(watched, event);
}

// -------------------------------------------------------
// copyTerminalSelection — copy the current selection in the terminal, but for
// any solution line the selection touches, copy the clean alg (no [count]
// brackets, plain ASCII — same as "Copy alg") instead of the displayed glyphs.
// Non-solution lines (e.g. "searching depth 10") copy their selected substring.
// -------------------------------------------------------
bool MainWindow::copyTerminalSelection()
{
    if (!txtOutput)
        return false;
    QTextCursor cur = txtOutput->textCursor();
    if (!cur.hasSelection())
        return false;

    const int selStart = cur.selectionStart();
    const int selEnd = cur.selectionEnd();

    QStringList outLines;
    for (QTextBlock block = txtOutput->document()->findBlock(selStart);
         block.isValid() && block.position() <= selEnd;
         block = block.next())
    {
        const int bStart = block.position();
        const int bEnd = bStart + block.text().length();
        // Skip a zero-length boundary block the selection only abuts.
        if (bStart == selEnd && selStart != selEnd)
            break;

        if (AlgBlockData *data = dynamic_cast<AlgBlockData *>(block.userData()))
        {
            // Any overlap with this solution line → emit the whole clean alg.
            int lb = data->rawLine.lastIndexOf('[');
            outLines << (lb > 0 ? data->rawLine.left(lb).trimmed()
                                : data->rawLine.trimmed());
        }
        else
        {
            int s = qMax(selStart, bStart);
            int e = qMin(selEnd, bEnd);
            outLines << (e > s ? block.text().mid(s - bStart, e - s) : QString());
        }
    }

    QApplication::clipboard()->setText(outLines.join('\n'));
    return true;
}

// -------------------------------------------------------
// showAlgContextMenu — alg context menu from table or terminal
// -------------------------------------------------------
void MainWindow::showAlgContextMenu(const QPoint &globalPos, const QString &rawLine)
{
    QString menuBg = Theme::primaryBg();
    QString menuBorder = Theme::borderGroup();
    QString menuText = Theme::textPrimary();
    QString menuSel = Theme::hoverBg();

    QMenu menu;
    menu.setStyleSheet(QString(
                           "QMenu { background: %1; border: 1px solid %2; border-radius: 6px; padding: 4px; color: %3; font-size: 12px; }"
                           "QMenu::item { padding: 6px 16px; border-radius: 4px; }"
                           "QMenu::item:selected { background: %4; }")
                           .arg(menuBg, menuBorder, menuText, menuSel));

    QAction *copyAct = menu.addAction("⧉  Copy alg");
    QAction *favAct = menu.addAction("♥  Add to Favorites Bin");

    QAction *chosen = menu.exec(globalPos);
    if (chosen == copyAct)
    {
        // Strip brackets for clipboard copy
        int lb = rawLine.lastIndexOf('[');
        QString alg = lb > 0 ? rawLine.left(lb).trimmed() : rawLine.trimmed();
        QApplication::clipboard()->setText(alg);
    }
    else if (chosen == favAct)
    {
        // With Abid notation on, store the abidified alg (PUA glyphs) so the bin
        // renders it with the Kompact font. The [count] suffix is left as-is.
        QString toStore = rawLine;
        if (m_abidNotation && !OutputConverter::s_abidFontFamily.isEmpty())
        {
            int lb = rawLine.lastIndexOf('[');
            toStore = lb > 0
                          ? OutputConverter::abidifyDisplay(rawLine.left(lb)) + rawLine.mid(lb)
                          : OutputConverter::abidifyDisplay(rawLine);
        }
        addToFavoritesBin(toStore);
    }
}

// -------------------------------------------------------
// addToFavoritesBin
// -------------------------------------------------------
void MainWindow::addToFavoritesBin(const QString &algLine)
{
    if (m_lastRunKey.isEmpty())
        return;
    QStringList &bin = m_favorites[m_lastRunKey];
    if (!bin.contains(algLine))
    {
        bin.append(algLine);
        saveSettings();
    }
}

// -------------------------------------------------------
// saveSettings / loadSettings — persist the favorites bins, the run config
// (buildArgList output, applied via applyRunConfig on load), and the display
// options. The sq1widget (cube) state is intentionally NOT persisted.
// -------------------------------------------------------
void MainWindow::saveSettings()
{
    QSettings s("Sq1Opt", "sq1opt-ui");

    // ── Favorites bins ────────────────────────────────────────────────────────
    s.beginGroup("favorites");
    s.remove("");
    s.setValue("count", m_favorites.size());
    int i = 0;
    for (auto it = m_favorites.constBegin(); it != m_favorites.constEnd(); ++it, ++i)
    {
        s.setValue(QString("bin%1_key").arg(i), it.key());
        s.setValue(QString("bin%1_algs").arg(i), it.value());
        if (m_favNames.contains(it.key()))
            s.setValue(QString("bin%1_name").arg(i), m_favNames[it.key()]);
    }
    s.endGroup();

    // ── Run config (solver flags) ─────────────────────────────────────────────
    s.beginGroup("runconfig");
    s.setValue("flags", buildArgList());
    s.endGroup();

    // ── Display options (set separately from the run config) ──────────────────
    s.beginGroup("display");
    s.setValue("smartKarn", m_smartKarn);
    s.setValue("abidNotation", m_abidNotation);
    s.setValue("debugOutput", m_debugOutput);
    s.setValue("karnotation", chkKarnotation->isChecked());
    s.setValue("normalizeAbfMode", m_normalizeAbfMode);
    s.setValue("inputMode", m_inputModeIndex);
    s.endGroup();
}

void MainWindow::loadSettings()
{
    QSettings s("Sq1Opt", "sq1opt-ui");

    // ── Favorites bins ────────────────────────────────────────────────────────
    s.beginGroup("favorites");
    int count = s.value("count", 0).toInt();
    for (int i = 0; i < count; i++)
    {
        QString key = s.value(QString("bin%1_key").arg(i)).toString();
        QStringList algs = s.value(QString("bin%1_algs").arg(i)).toStringList();
        QString name = s.value(QString("bin%1_name").arg(i)).toString();
        if (!key.isEmpty())
        {
            m_favorites[key] = algs;
            if (!name.isEmpty())
                m_favNames[key] = name;
        }
    }
    s.endGroup();

    // ── Display options ───────────────────────────────────────────────────────
    s.beginGroup("display");
    if (s.contains("smartKarn"))
        m_smartKarn = s.value("smartKarn").toBool();
    if (s.contains("debugOutput"))
        m_debugOutput = s.value("debugOutput").toBool();
    // Abid display only meaningful when the font actually loaded.
    if (s.contains("abidNotation"))
        m_abidNotation = s.value("abidNotation").toBool() && !OutputConverter::s_abidFontFamily.isEmpty();
    if (s.contains("karnotation"))
        chkKarnotation->setChecked(s.value("karnotation").toBool());
    if (s.contains("normalizeAbfMode"))
    {
        m_normalizeAbfMode = s.value("normalizeAbfMode").toInt();
        if (m_normalizeAbfGroup && m_normalizeAbfGroup->button(m_normalizeAbfMode))
            m_normalizeAbfGroup->button(m_normalizeAbfMode)->setChecked(true);
    }
    if (s.contains("inputMode") && m_inputMode && m_mainInput)
    {
        m_inputModeIndex = s.value("inputMode").toInt();
        if (m_inputModeIndex == 1)
        {
            m_inputMode->setText("ALG");
            m_mainInput->setPlaceholderText("1,0 / 3,3 / 0,-3 / ...  (supports karn)");
        }
        else if (m_inputModeIndex == 2)
        {
            m_inputMode->setText("POSITION");
            m_mainInput->setPlaceholderText("ABCDEFGH12345678-");
        }
        else
        {
            m_inputModeIndex = 0;
            m_inputMode->setText("SCRAMBLE");
            m_mainInput->setPlaceholderText("1,0 / 3,3 / 0,-3 / ...  (supports karn)");
        }
    }
    s.endGroup();

    // ── Run config: apply saved flags to the UI via applyRunConfig. Keep the
    // current (default) cube position — the cube state itself is not persisted. ─
    s.beginGroup("runconfig");
    QStringList flags = s.value("flags").toStringList();
    s.endGroup();
    if (!flags.isEmpty())
        applyRunConfig(cubeWidget->getPositionString() + " " + flags.join(" "));
}

// -------------------------------------------------------
// applyRunConfig — parse "POS flags..." key and apply to UI
// -------------------------------------------------------
void MainWindow::applyRunConfig(const QString &key)
{
    QStringList parts = key.split(' ', Qt::SkipEmptyParts);
    if (parts.isEmpty())
        return;

    // First token is position string, rest are flags
    QString pos = parts[0];
    QStringList flags = parts.mid(1);

    cubeWidget->setPositionFromString(pos);

    // Metric radio
    if (m_metricGroup)
    {
        if (flags.contains("-es"))
            m_metricGroup->button(0)->setChecked(true);
        else if (flags.contains("-ea"))
            m_metricGroup->button(2)->setChecked(true);
        else
            m_metricGroup->button(1)->setChecked(true);
    }

    // All optimal / suboptimal
    bool hasA = false;
    int aVal = 0;
    for (const QString &f : flags)
    {
        if (f == "-a")
        {
            hasA = true;
            break;
        }
        if (f.startsWith("-a") && f.length() > 2)
        {
            bool ok;
            int v = f.mid(2).toInt(&ok);
            if (ok)
            {
                hasA = true;
                aVal = v;
                break;
            }
        }
    }
    chkAllOptimal->setChecked(hasA);
    if (hasA && aVal > 0)
        spnSuboptimal->setValue(aVal);
    else if (hasA)
        spnSuboptimal->setValue(0);

    // Specific depths
    bool hasD = false;
    QString depStr;
    for (const QString &f : flags)
    {
        if (f.startsWith("-d") && f.length() > 2)
        {
            hasD = true;
            depStr = f.mid(2);
            break;
        }
    }
    chkDepths->setChecked(hasD);
    if (hasD)
        txtDepths->setText(depStr);

    // Generator
    chkGenerator->setChecked(flags.contains("-g"));

    // TwoGen radio
    if (m_twoGenGroup)
    {
        if (flags.contains("-2"))
            m_twoGenGroup->button(0)->setChecked(true);
        else if (flags.contains("-p"))
            m_twoGenGroup->button(1)->setChecked(true);
        else
            m_twoGenGroup->button(2)->setChecked(true);
    }

    // Cubeshape / equator
    chkCubeshape->setChecked(flags.contains("-c"));
    chkIgnoreEquator->setChecked(flags.contains("-m"));

    // Angle lock radio
    if (m_angleGroup)
    {
        if (flags.contains("-nb"))
            m_angleGroup->button(0)->setChecked(true);
        else if (flags.contains("-nu"))
            m_angleGroup->button(1)->setChecked(true);
        else if (flags.contains("-nd"))
            m_angleGroup->button(2)->setChecked(true);
        else
            m_angleGroup->button(3)->setChecked(true);
    }

    // Max limits
    auto parseLimit = [&](const QString &prefix, QCheckBox *chk, QSpinBox *spn)
    {
        bool found = false;
        int val = 0;
        for (const QString &f : flags)
            if (f.startsWith(prefix) && f.length() > prefix.length())
            {
                bool ok;
                val = f.mid(prefix.length()).toInt(&ok);
                if (ok)
                {
                    found = true;
                    break;
                }
            }
        chk->setChecked(found);
        if (found)
            spn->setValue(val);
    };
    parseLimit("-X", chkMaxX, spnMaxX);
    parseLimit("-Y", chkMaxY, spnMaxY);
    parseLimit("-Z", chkMaxTotal, spnMaxTotal);

    // Ignore trans
    m_ignoreTrans = flags.contains("-x");
    if (chkIgnoreTransSetting)
        chkIgnoreTransSetting->setChecked(m_ignoreTrans);

    updateConstraints();
    updateCommand();
    cubeWidget->update();

    // Clear terminal state
    m_rawLines.clear();
    m_karnLines.clear();
    m_solutionLines.clear();
    m_karnSolutionLines.clear();
    m_solutionLinesForRating.clear();
    m_sliceIndicators.clear();
    m_rawFinalScores.clear();
    m_cachedRatedOrder.clear();
    m_ratingsValid = false;
    m_seenSolutions.clear();
    m_seenNormalizedAlgs.clear();
    m_debugBuffer.clear();
    m_algAnnLines.clear();
    m_tableVisible = false;
    txtOutput->setVisible(true);
    m_tableContainer->setVisible(false);
    btnTableMode->setText("⊞");
    btnTableMode->setToolTip("Switch to table view");
    // Hide output buttons — no solutions after apply
    btnCopyTerminal->setVisible(false);
    btnFavorites->setVisible(true);
    rebuildTerminalView();
}
