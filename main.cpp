#include <QApplication>
#include <QMainWindow>
#include <QWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QPushButton>
#include <QLineEdit>
#include <QLabel>
#include <QProgressBar>
#include <QListWidget>
#include <QListWidgetItem>
#include <QDialog>
#include <QTabWidget>
#include <QFormLayout>
#include <QRadioButton>
#include <QComboBox>
#include <QFileDialog>
#include <QScrollArea>
#include <QStyle>
#include <QProcess>
#include <QIcon>
#include <QPixmap>
#include <QMouseEvent>
#include <QDebug>
#include <QRegularExpression>
#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkReply>
#include <QDesktopServices>
#include <QUrl>
#include <QClipboard>
#include <QTextEdit>
#include <QPainter>
#include <QBrush>
#include <QStandardPaths>
#include <QFile>
#include <QTimer>
#include <QCheckBox>
#include <QScrollBar>
#include <QGroupBox>
#include <QIntValidator>
#include <QDoubleValidator>
#include <memory>
#ifdef Q_OS_WIN
#include <windows.h>
#endif

struct AppSettings {
    QString defaultFormat = "MP4";
    QString defaultQuality = "1080p";
    QString defaultAudioFormat = "MP3";
    QString defaultAudioQuality = "128 kbps";
    QString downloadPath = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
    int maxConcurrent = 3;
    double speedLimit = 0.0;
    bool createPlaylistFolder = true;
};

struct VideoMetadata {
    QString title;
    QString description;
    QString channel;
    QString thumbnailUrl;
    QString channelThumbUrl;
    QString filePath;
};

// --- Custom Widget para o Card de Download ---
class DownloadCard : public QWidget {
    Q_OBJECT
signals:
    void metadataLoaded();
    void requestRemoval();
    void finished(); // Novo sinal para controle de fila
    void clicked(DownloadCard* card);
    void progressUpdated(int value);

public:
    enum Status { Waiting, Analyzing, Downloading, Completed, Canceled, Error };

    DownloadCard(QString url, bool isMini = false, QWidget* parent = nullptr) 
        : QWidget(parent), process(new QProcess(this)), videoUrl(url), 
          netManager(new QNetworkAccessManager(this)), isPaused(false), isCanceled(false),
          m_status(Waiting), m_isMini(isMini) {
        
        if (m_isMini) setFixedHeight(65);
        else setFixedHeight(100);

        QHBoxLayout* layout = new QHBoxLayout(this);

        // Miniatura (Placeholder)
        thumbLabel = new QLabel();
        thumbLabel->setObjectName("thumbLabel");
        if (m_isMini) thumbLabel->setFixedSize(80, 45);
        else thumbLabel->setFixedSize(140, 80);
        thumbLabel->setStyleSheet("background-color: #333; border-radius: 4px;");
        thumbLabel->setAlignment(Qt::AlignCenter);
        thumbLabel->setText("Thumb");

        // Informações centrais
        QVBoxLayout* infoLayout = new QVBoxLayout();
        titleLabel = new QLabel("Carregando informações...");
        titleLabel->setStyleSheet(m_isMini ? "font-weight: bold; font-size: 11px; color: white;" : "font-weight: bold; font-size: 14px; color: white;");
        
        progressBar = new QProgressBar();
        progressBar->setRange(0, 100);
        progressBar->setValue(0);
        progressBar->setTextVisible(false);
        progressBar->setFixedHeight(m_isMini ? 8 : 15);

        speedLabel = new QLabel("Aguardando na fila...");
        speedLabel->setStyleSheet("font-size: 11px; color: #aaa;");

        infoLayout->addWidget(titleLabel);
        infoLayout->addWidget(progressBar);
        infoLayout->addWidget(speedLabel);

        // Botões de controle (Direita)
        QVBoxLayout* buttonLayout = new QVBoxLayout();
        pauseBtn = new QPushButton();
        pauseBtn->setIcon(style()->standardIcon(QStyle::SP_MediaPause));
        
        cancelBtn = new QPushButton();
        cancelBtn->setIcon(style()->standardIcon(QStyle::SP_DialogCancelButton));
        
        playBtn = new QPushButton();
        playBtn->setIcon(style()->standardIcon(QStyle::SP_MediaPlay));
        playBtn->hide(); // Escondido até terminar

        buttonLayout->addWidget(pauseBtn);
        buttonLayout->addWidget(cancelBtn);
        buttonLayout->addWidget(playBtn);
        
        // Funcionalidade do Botão Pausar
        connect(pauseBtn, &QPushButton::clicked, [this]() {
            if (!isPaused) {
                if (process->state() == QProcess::Running) {
                    process->kill(); // Interrompe para pausar
                }
                isPaused = true;
                pauseBtn->setIcon(style()->standardIcon(QStyle::SP_MediaPlay));
                speedLabel->setText("Pausado");
            } else {
                isPaused = false;
                pauseBtn->setIcon(style()->standardIcon(QStyle::SP_MediaPause));
                // Reinicia o download. O yt-dlp continuará automaticamente de onde parou.
                startDownload(videoUrl, savedPath, savedIsVideo, savedFormat, savedQuality);
            }
        });

        // Funcionalidade do Botão Cancelar
        connect(cancelBtn, &QPushButton::clicked, [this]() {
            isCanceled = true;
            process->kill();
            speedLabel->setText("Cancelado");
            QTimer::singleShot(1000, this, [this]() {
                emit finished();
                emit requestRemoval();
            });
        });

        connect(playBtn, &QPushButton::clicked, [this]() {
            QDesktopServices::openUrl(QUrl::fromLocalFile(metadata.filePath));
        });

        layout->addWidget(thumbLabel);
        layout->addLayout(infoLayout);
        layout->addLayout(buttonLayout);

        setStyleSheet("DownloadCard { border: 1px solid #333; border-radius: 5px; background: #2d2d2d; }");
    }

    void mousePressEvent(QMouseEvent* event) override {
        emit clicked(this);
        QWidget::mousePressEvent(event);
    }

    QString getUrl() const { return videoUrl; }
    Status status() const { return m_status; }
    VideoMetadata getMetadata() const { return metadata; }

    void setInitialMetadata(const VideoMetadata& meta) {
        this->metadata = meta;
        if (!meta.title.isEmpty()) titleLabel->setText(meta.title);
        if (!meta.thumbnailUrl.isEmpty()) loadThumbnail(meta.thumbnailUrl);
    }

    void startDownload(QString url, QString path, bool isVideo, QString format, QString quality) {
        // Salva os parâmetros para permitir a retomada (resume)
        this->savedPath = path;
        this->savedIsVideo = isVideo;
        this->savedFormat = format;
        this->savedQuality = quality;
        this->videoUrl = url;

        QString binDir = QCoreApplication::applicationDirPath() + "/ffmpeg/";
        QString ytDlpPath = binDir + "yt-dlp.exe";
        QString ffmpegPath = binDir + "ffmpeg.exe";

        process->disconnect(); // Evita múltiplas conexões ao reiniciar
        m_status = Analyzing;
        speedLabel->setText("Analisando...");

        // Se já temos o título, não precisamos pedir o JSON de novo (Resume)
        if (!metadata.title.isEmpty()) {
            executeDownload(url, path, isVideo, format, quality, ytDlpPath, ffmpegPath);
            return;
        }

        if (!QFile::exists(ytDlpPath)) {
            showErrorDialog("Erro de Configuração", "O executável yt-dlp.exe não foi encontrado na pasta ffmpeg.", "Caminho esperado: " + ytDlpPath);
            return;
        }

        // Configura para não mostrar terminal
        #ifdef Q_OS_WIN
        process->setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *args) {
            args->flags |= CREATE_NO_WINDOW;
        });
        #endif

        QStringList args;
        // Primeiro: Obter Metadados em JSON
        args << "--dump-json" << "--no-playlist" << url;

        connect(process, &QProcess::finished, [this, url, path, isVideo, format, quality, ytDlpPath, ffmpegPath](int exitCode) {
            if (exitCode == 0) {
                // Se terminou a fase de JSON, inicia o download real
                if (process->arguments().contains("--dump-json")) {
                    parseJsonAndDownload(url, path, isVideo, format, quality, ytDlpPath, ffmpegPath);
                }
            } else {
                QString errorData = process->readAllStandardError();
                showErrorDialog("Erro ao obter informações", "O yt-dlp falhou ao analisar o vídeo. Verifique o link.", errorData);
            }
        });

        process->start(ytDlpPath, args);
    }

    void parseJsonAndDownload(QString url, QString path, bool isVideo, QString format, QString quality, QString ytDlp, QString ffmpeg) {
        QByteArray data = process->readAllStandardOutput();
        QJsonDocument doc = QJsonDocument::fromJson(data);
        QJsonObject obj = doc.object();

        metadata.title = obj["title"].toString();
        metadata.description = obj["description"].toString();
        metadata.channel = obj["uploader"].toString();
        metadata.thumbnailUrl = obj["thumbnail"].toString();
        
        // Tenta encontrar o avatar do canal nos thumbnails (geralmente o que tem proporção 1:1)
        QJsonArray thumbnails = obj["thumbnails"].toArray();
        for (const QJsonValue &v : thumbnails) {
            QJsonObject t = v.toObject();
            QString tid = t["id"].toString();
            QString turl = t["url"].toString();
            // Lógica expandida para pegar o avatar do canal
            if (turl.contains("yt3.ggpht.com") || tid.contains("avatar") || 
                (t["width"].toInt() == t["height"].toInt() && t["width"].toInt() > 0)) {
                metadata.channelThumbUrl = turl;
                break;
            }
        }

        // Define o caminho de saída esperado
        metadata.filePath = QDir(path).absoluteFilePath(metadata.title + "." + format.toLower());

        titleLabel->setText(metadata.title);
        loadThumbnail(metadata.thumbnailUrl);
        
        emit metadataLoaded();
        executeDownload(url, path, isVideo, format, quality, ytDlp, ffmpeg);
    }

    void executeDownload(QString url, QString path, bool isVideo, QString format, QString quality, QString ytDlp, QString ffmpeg) {
        m_status = Downloading;
        // Configura download real
        process->disconnect(); 
        
        QStringList args;
        args << "--ffmpeg-location" << ffmpeg << "--newline" << "--no-warnings" << "-o" << path + "/%(title)s.%(ext)s";
        
        if (isVideo) {
            QString res = quality.split(" ").first().replace("p", "");
            args << "-f" << QString("bestvideo[height<=?%1]+bestaudio/best").arg(res) << "--merge-output-format" << format.toLower();
        } else {
            QString kbps = quality.split(" ").first();
            args << "-x" << "--audio-format" << format.toLower() 
                 << "--audio-quality" << kbps + "K"
                 << "--embed-thumbnail" << "--embed-metadata";
        }
        args << url;

        #ifdef Q_OS_WIN
        process->setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *args) {
            args->flags |= CREATE_NO_WINDOW;
        });
        #endif

        connect(process, &QProcess::readyReadStandardOutput, this, &DownloadCard::handleProgress);
        connect(process, &QProcess::finished, [this](int exitCode, QProcess::ExitStatus exitStatus) {
            // Se foi pausado ou cancelado manualmente, ignoramos o código de erro do encerramento do processo
            if (isPaused || isCanceled) return;

            if (exitCode == 0) {
                m_status = Completed;
                progressBar->setValue(100);
                speedLabel->setText("Download concluído!");
                pauseBtn->hide();
                cancelBtn->hide();
                playBtn->show();
            } else {
                m_status = Error;
                showErrorDialog("Erro de Download", "Ocorreu uma falha durante o download do conteúdo.", process->readAllStandardError());
            }
            emit finished(); // Notifica o gerenciador para iniciar o próximo
        });

        process->start(ytDlp, args);
    }

    static void showErrorDialog(QString title, QString msg, QString rawError) {
        QDialog* diag = new QDialog();
        diag->setWindowTitle(title);
        diag->setMinimumWidth(500);
        QVBoxLayout* l = new QVBoxLayout(diag);
        l->addWidget(new QLabel(msg));
        
        if (!rawError.isEmpty()) {
            QTextEdit* edit = new QTextEdit(rawError);
            edit->setReadOnly(true);
            edit->setStyleSheet("font-family: monospace; background: #222; color: #0f0;");
            l->addWidget(new QLabel("Informações Brutas (Logs):"));
            l->addWidget(edit);
            
            QPushButton* copy = new QPushButton("Copiar Logs");
            connect(copy, &QPushButton::clicked, [rawError]() {
                QGuiApplication::clipboard()->setText(rawError);
            });
            l->addWidget(copy);
        }
        
        QPushButton* ok = new QPushButton("Fechar");
        connect(ok, &QPushButton::clicked, diag, &QDialog::accept);
        l->addWidget(ok);
        diag->exec();
    }

    void handleProgress() {
        while (process->canReadLine()) {
            QString output = process->readLine();
            QRegularExpression progRegex("\\[download\\]\\s+(\\d+\\.\\d+)%\\s+of.*at\\s+([\\d\\.\\w/]+)");
            QRegularExpressionMatch match = progRegex.match(output);
            if (match.hasMatch()) {
                updateProgress(static_cast<int>(match.captured(1).toFloat()), match.captured(2));
            emit progressUpdated(progressBar->value());
            }
        }
    }

    void loadThumbnail(QString url) {
        QNetworkReply* reply = netManager->get(QNetworkRequest(QUrl(url)));
        connect(reply, &QNetworkReply::finished, [this, reply]() {
            if (reply->error() == QNetworkReply::NoError) {
                QPixmap pix;
                pix.loadFromData(reply->readAll());
                fullPixmap = pix;
                thumbLabel->setPixmap(pix.scaled(thumbLabel->size(), Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation));
                emit metadataLoaded(); // Notifica para atualizar a lateral
            }
            reply->deleteLater();
        });
    }

    QPixmap getThumbnailPixmap() const {
        return fullPixmap.isNull() ? (thumbLabel->pixmap(Qt::ReturnByValue).isNull() ? QPixmap() : thumbLabel->pixmap(Qt::ReturnByValue)) : fullPixmap;
    }

    void updateProgress(int value, QString speed) {
        progressBar->setValue(value);
        speedLabel->setText("Velocidade: " + speed);
    }

    void setSelected(bool selected) {
        if (selected)
            setStyleSheet("DownloadCard { border: 2px solid #0078d7; background: #3e3e42; }");
        else
            setStyleSheet("DownloadCard { border: 1px solid #333; background: #2d2d2d; }");
    }

private:
    QLabel* thumbLabel;
    QLabel* titleLabel;
    QLabel* speedLabel;
    QProgressBar* progressBar;
    QPixmap fullPixmap;
    QPushButton* pauseBtn;
    QPushButton* cancelBtn;
    QPushButton* playBtn;
    QProcess* process;
    QString videoUrl;
    VideoMetadata metadata;
    QNetworkAccessManager* netManager;
    bool isPaused, isCanceled, m_isMini;
    Status m_status;
    // Membros para salvar o estado do download
    QString savedPath, savedFormat, savedQuality;
    bool savedIsVideo;
};

// --- Card de Playlist (Pasta) ---
class PlaylistCard : public QWidget {
    Q_OBJECT
public:
    PlaylistCard(QString title, QStringList videoUrls, QWidget* parent = nullptr) 
        : QWidget(parent), m_title(title), m_urls(videoUrls), m_isExpanded(false) {
        
        QVBoxLayout* mainLayout = new QVBoxLayout(this);
        mainLayout->setContentsMargins(0,0,0,0);
        mainLayout->setSpacing(0);

        // Header (O Card da Pasta)
        headerWidget = new QWidget();
        headerWidget->setFixedHeight(110);
        headerWidget->setStyleSheet("background: #333; border-radius: 6px; border: 1px solid #444;");
        QHBoxLayout* hLayout = new QHBoxLayout(headerWidget);

        // Thumbnails Empilhadas
        thumbStack = new QLabel();
        thumbStack->setFixedSize(170, 90);
        hLayout->addWidget(thumbStack);

        QVBoxLayout* info = new QVBoxLayout();
        QLabel* tLabel = new QLabel(title);
        tLabel->setStyleSheet("font-weight: bold; font-size: 16px; color: white;");
        
        countLabel = new QLabel(QString("%1 vídeos").arg(videoUrls.size()));
        countLabel->setStyleSheet("color: #aaa;");

        globalProgress = new QProgressBar();
        globalProgress->setFixedHeight(10);
        globalProgress->setStyleSheet("QProgressBar { background: #111; border-radius: 5px; } QProgressBar::chunk { background: #0078d7; }");

        info->addWidget(tLabel);
        info->addWidget(countLabel);
        info->addWidget(globalProgress);
        hLayout->addLayout(info);

        // Container de Vídeos (Escondido por padrão)
        listContainer = new QWidget();
        listLayout = new QVBoxLayout(listContainer);
        listLayout->setContentsMargins(40, 5, 10, 10); // Recuo para parecer subpasta
        listContainer->hide();

        mainLayout->addWidget(headerWidget);
        mainLayout->addWidget(listContainer);

        headerWidget->installEventFilter(this);
    }

    void addCard(DownloadCard* card) {
        listLayout->addWidget(card);
        m_cards.append(card);
        connect(card, &DownloadCard::progressUpdated, this, &PlaylistCard::updateAggregateProgress);
    }

    void setThumbs(const QList<QPixmap>& pixmaps) {
        QPixmap canvas(170, 90);
        canvas.fill(Qt::transparent);
        QPainter painter(&canvas);
        
        // Desenha de trás para frente com offset
        for(int i = qMin(pixmaps.size() - 1, 2); i >= 0; --i) {
            painter.drawPixmap(i * 15, (2-i) * 5, pixmaps[i].scaled(130, 75, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation));
        }
        thumbStack->setPixmap(canvas);
    }

    const QList<DownloadCard*>& getCards() const { return m_cards; }

    bool eventFilter(QObject* obj, QEvent* event) override {
        if (obj == headerWidget && event->type() == QEvent::MouseButtonPress) {
            toggleExpand();
            return true;
        }
        return QWidget::eventFilter(obj, event);
    }

signals:
    void sizeChanged();

private:
    void toggleExpand() {
        m_isExpanded = !m_isExpanded;
        listContainer->setVisible(m_isExpanded);
        emit sizeChanged();
    }

    void updateAggregateProgress() {
        if (m_cards.isEmpty()) return;
        double total = 0;
        for (auto* c : m_cards) total += c->findChild<QProgressBar*>()->value();
        globalProgress->setValue(static_cast<int>(total / m_cards.size()));
    }

    QString m_title;
    QStringList m_urls;
    bool m_isExpanded;
    QWidget *headerWidget, *listContainer;
    QVBoxLayout *listLayout;
    QLabel *thumbStack, *countLabel;
    QProgressBar *globalProgress;
    QList<DownloadCard*> m_cards;
};

struct PlaylistData {
    QString title;
    struct Entry { QString url; QString title; QString thumb; };
    QList<Entry> entries;
};

// --- Janela de Adicionar Download ---
class AddDownloadDialog : public QDialog {
    Q_OBJECT
public:
    AddDownloadDialog(AppSettings settings, QWidget* parent = nullptr) : QDialog(parent), m_settings(settings) {
        setWindowTitle("Novo Download - Yout8");
        setFixedWidth(450);
        QVBoxLayout* layout = new QVBoxLayout(this);

        urlInput = new QLineEdit();
        urlInput->setPlaceholderText("Cole o link do YouTube aqui...");
        
        QHBoxLayout* typeLayout = new QHBoxLayout();
        videoRadio = new QRadioButton("Vídeo");
        audioRadio = new QRadioButton("Áudio");
        videoRadio->setChecked(true);
        typeLayout->addWidget(videoRadio);
        typeLayout->addWidget(audioRadio);

        formatCombo = new QComboBox();
        qualityCombo = new QComboBox();
        updateFormats(true);

        pathInput = new QLineEdit(QStandardPaths::writableLocation(QStandardPaths::DownloadLocation));
        QPushButton* browseBtn = new QPushButton("...");
        QHBoxLayout* pathLayout = new QHBoxLayout();
        pathLayout->addWidget(new QLabel("Salvar em:"));
        pathLayout->addWidget(pathInput);
        pathLayout->addWidget(browseBtn);

        QHBoxLayout* actions = new QHBoxLayout();
        QPushButton* downloadBtn = new QPushButton("Baixar");
        downloadBtn->setStyleSheet("background-color: #0078d7; color: white; font-weight: bold; padding: 8px;");
        QPushButton* cancelBtn = new QPushButton("Cancelar");
        actions->addWidget(downloadBtn);
        actions->addWidget(cancelBtn);

        layout->addWidget(new QLabel("Link:"));
        layout->addWidget(urlInput);
        layout->addLayout(typeLayout);
        layout->addWidget(new QLabel("Formato:"));
        layout->addWidget(formatCombo);
        layout->addWidget(new QLabel("Qualidade:"));
        layout->addWidget(qualityCombo);
        layout->addLayout(pathLayout);
        layout->addSpacing(10);
        layout->addLayout(actions);

        connect(videoRadio, &QRadioButton::toggled, this, &AddDownloadDialog::updateFormats);
        connect(browseBtn, &QPushButton::clicked, [this]() {
            QString dir = QFileDialog::getExistingDirectory(this, "Selecionar Pasta");
            if (!dir.isEmpty()) pathInput->setText(dir);
        });
        connect(cancelBtn, &QPushButton::clicked, this, &QDialog::reject);
        connect(downloadBtn, &QPushButton::clicked, this, &QDialog::accept);
    }

    QString getUrl() { return urlInput->text(); }
    QString getPath() { return pathInput->text(); }
    bool isVideo() { return videoRadio->isChecked(); }
    QString getFormat() { return formatCombo->currentText(); }
    QString getQuality() { return qualityCombo->currentText(); }

private slots:
    void updateFormats(bool isVideo) {
        formatCombo->clear();
        qualityCombo->clear();
        if (isVideo) {
            formatCombo->addItems({"MP4", "MKV", "WEBM", "AVI", "MOV", "FLV", "WMV", "M4V"});
            qualityCombo->addItems({"2160p (4K)", "1440p (2K)", "1080p", "720p", "480p", "360p", "240p", "144p"});
            if (formatCombo->findText(m_settings.defaultFormat) != -1) formatCombo->setCurrentText(m_settings.defaultFormat);
            if (qualityCombo->findText(m_settings.defaultQuality) != -1) qualityCombo->setCurrentText(m_settings.defaultQuality);
        } else {
            formatCombo->addItems({"MP3", "M4A", "WAV", "FLAC", "OGG", "OPUS", "WMA", "AAC"});
            qualityCombo->addItems({"320 kbps", "256 kbps", "192 kbps", "160 kbps", "128 kbps", "96 kbps", "64 kbps", "32 kbps"});
            if (formatCombo->findText(m_settings.defaultAudioFormat) != -1) formatCombo->setCurrentText(m_settings.defaultAudioFormat);
            if (qualityCombo->findText(m_settings.defaultAudioQuality) != -1) qualityCombo->setCurrentText(m_settings.defaultAudioQuality);
        }
    }

private:
    QLineEdit *urlInput, *pathInput;
    QRadioButton *videoRadio, *audioRadio;
    QComboBox *formatCombo, *qualityCombo;
    AppSettings m_settings;
};

// --- Janela Principal ---
class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    MainWindow() : m_activeDownloads(0), m_netManager(new QNetworkAccessManager(this)), m_selectedCard(nullptr) {
        setWindowTitle("Yout8 - YouTube Downloader");
        resize(1000, 600);

        QWidget* central = new QWidget();
        central->setStyleSheet("background-color: #1e1e1e; color: white;");
        
        QHBoxLayout* mainLayout = new QHBoxLayout(central);
        mainLayout->setContentsMargins(0,0,0,0);
        mainLayout->setSpacing(0);

        // --- Lateral Esquerda (Detalhes) ---
        QWidget* leftPanel = new QWidget();
        leftPanel->setFixedWidth(350);
        leftPanel->setStyleSheet("background-color: #1e1e1e; border-right: 1px solid #333;");
        QVBoxLayout* leftLayout = new QVBoxLayout(leftPanel);

        QPushButton* addBtn = new QPushButton();
        addBtn->setIcon(style()->standardIcon(QStyle::SP_FileDialogNewFolder));
        addBtn->setFixedSize(40, 40);
        leftLayout->addWidget(addBtn, 0, Qt::AlignLeft);

        detailThumb = new QLabel();
        detailThumb->setFixedSize(330, 185);
        detailThumb->setStyleSheet("background-color: #eee; border: 1px solid #ccc;");
        detailThumb->setAlignment(Qt::AlignCenter);
        detailThumb->setText("Selecione um vídeo");

        detailTitle = new QLabel("Título do Vídeo");
        detailTitle->setWordWrap(true);
        detailTitle->setStyleSheet("font-size: 16px; font-weight: bold; margin-top: 10px; color: white;");

        QHBoxLayout* channelLayout = new QHBoxLayout();
        channelPic = new QLabel();
        channelPic->setFixedSize(40, 40);
        channelPic->setStyleSheet("background-color: #333; border-radius: 20px;");
        channelName = new QLabel("Nome do Canal");
        channelName->setStyleSheet("color: #ddd; font-weight: bold;");
        channelLayout->addWidget(channelPic);
        channelLayout->addWidget(channelName);
        channelLayout->addStretch();

        detailDesc = new QLabel("Descrição aparecerá aqui...");
        detailDesc->setWordWrap(true);
        detailDesc->setAlignment(Qt::AlignTop);
        detailDesc->setStyleSheet("color: #aaa; font-size: 12px;");

        leftLayout->addWidget(detailThumb);
        leftLayout->addWidget(detailTitle);
        leftLayout->addLayout(channelLayout);
        leftLayout->addWidget(detailDesc);
        leftLayout->addStretch();

        // --- Painel Direito ---
        QWidget* rightPanel = new QWidget();
        QVBoxLayout* rightLayout = new QVBoxLayout(rightPanel);
        rightLayout->setContentsMargins(0, 0, 0, 0);
        rightLayout->setSpacing(0);

        // Barra Superior Direita (Botão Config)
        QWidget* topBar = new QWidget();
        topBar->setFixedHeight(50);
        QHBoxLayout* topBarLayout = new QHBoxLayout(topBar);
        topBarLayout->addStretch();
        
        QPushButton* settingsBtn = new QPushButton();
        settingsBtn->setIcon(style()->standardIcon(QStyle::SP_FileDialogDetailedView));
        settingsBtn->setFixedSize(35, 35);
        settingsBtn->setToolTip("Configurações");
        settingsBtn->setStyleSheet("background: #2d2d2d; border: 1px solid #444; border-radius: 4px;");
        topBarLayout->addWidget(settingsBtn);

        downloadList = new QListWidget();
        downloadList->setSpacing(8);
        downloadList->setSelectionMode(QAbstractItemView::SingleSelection);
        downloadList->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
        downloadList->setStyleSheet("QListWidget { background-color: #1e1e1e; border: none; padding: 10px; } QListWidget::item { border: none; }");
        downloadList->verticalScrollBar()->setSingleStep(20); // Define a velocidade (valor menor = mais lento)

        rightLayout->addWidget(topBar);
        rightLayout->addWidget(downloadList);

        mainLayout->addWidget(leftPanel);
        mainLayout->addWidget(rightPanel);

        setCentralWidget(central);
        loadSettings();

        connect(addBtn, &QPushButton::clicked, this, &MainWindow::openAddDialog);
        connect(downloadList, &QListWidget::itemSelectionChanged, this, &MainWindow::onSelectionChanged);
        connect(settingsBtn, &QPushButton::clicked, this, &MainWindow::openSettingsDialog);
    }

    void loadChannelAvatar(QString url) {
        QNetworkAccessManager* mgr = new QNetworkAccessManager(this);
        QNetworkReply* reply = mgr->get(QNetworkRequest(QUrl(url)));
        connect(reply, &QNetworkReply::finished, [this, reply, mgr]() {
            if (reply->error() == QNetworkReply::NoError) {
                QPixmap pix;
                pix.loadFromData(reply->readAll());
                
                // Tornar a imagem circular
                int size = qMin(pix.width(), pix.height());
                QPixmap rounded(size, size);
                rounded.fill(Qt::transparent);
                QPainter painter(&rounded);
                painter.setRenderHint(QPainter::Antialiasing);
                QBrush brush(pix.scaled(size, size, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation));
                painter.setBrush(brush);
                painter.setPen(Qt::NoPen);
                painter.drawEllipse(0, 0, size, size);
                painter.end();

                channelPic->setPixmap(rounded.scaled(channelPic->size(), Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation));
            }
            reply->deleteLater();
            mgr->deleteLater();
        });
    }

private slots:
    void openSettingsDialog() {
        QDialog diag(this);
        diag.setWindowTitle("Preferências - Yout8");
        diag.setMinimumWidth(450);
        QVBoxLayout* mainL = new QVBoxLayout(&diag);

        QTabWidget* tabs = new QTabWidget();

        // --- Aba Padrão ---
        QWidget* tabPadrao = new QWidget();
        QVBoxLayout* padraoL = new QVBoxLayout(tabPadrao);
        QFormLayout* formPadrao = new QFormLayout();

        QComboBox* defFormat = new QComboBox();
        defFormat->addItems({"MP4", "MKV", "WEBM", "MP3", "M4A", "WAV", "FLAC"});
        defFormat->setCurrentText(m_settings.defaultFormat);

        QComboBox* defQual = new QComboBox();
        defQual->addItems({"2160p (4K)", "1440p (2K)", "1080p", "720p", "480p", "360p", "320 kbps", "256 kbps", "128 kbps"});
        defQual->setCurrentText(m_settings.defaultQuality);

        QComboBox* defAudioFormat = new QComboBox();
        defAudioFormat->addItems({"MP3", "M4A", "WAV", "FLAC", "OGG", "OPUS", "WMA", "AAC"});
        defAudioFormat->setCurrentText(m_settings.defaultAudioFormat);

        QComboBox* defAudioQual = new QComboBox();
        defAudioQual->addItems({"320 kbps", "256 kbps", "192 kbps", "160 kbps", "128 kbps", "96 kbps", "64 kbps", "32 kbps"});
        defAudioQual->setCurrentText(m_settings.defaultAudioQuality);

        QLineEdit* pathEd = new QLineEdit(m_settings.downloadPath);
        QPushButton* browseBtn = new QPushButton("...");
        QHBoxLayout* pathL = new QHBoxLayout();
        pathL->addWidget(pathEd);
        pathL->addWidget(browseBtn);

        formPadrao->addRow("Formato Padrão:", defFormat);
        formPadrao->addRow("Qualidade Padrão:", defQual);
        formPadrao->addRow("Formato Áudio Padrão:", defAudioFormat);
        formPadrao->addRow("Qualidade Áudio Padrão:", defAudioQual);
        formPadrao->addRow("Pasta de Downloads:", pathL);
        padraoL->addLayout(formPadrao);
        padraoL->addStretch();

        // --- Aba Download ---
        QWidget* tabDownload = new QWidget();
        QVBoxLayout* downL = new QVBoxLayout(tabDownload);
        
        QLineEdit* maxDown = new QLineEdit(QString::number(m_settings.maxConcurrent));
        maxDown->setValidator(new QIntValidator(1, 10, this));
        
        QLineEdit* speedLimit = new QLineEdit(QString::number(m_settings.speedLimit).replace(".", ","));
        speedLimit->setValidator(new QDoubleValidator(0.0, 1000.0, 2, this));

        QCheckBox* playlistFolder = new QCheckBox("Criar pasta automaticamente para playlists");
        playlistFolder->setChecked(m_settings.createPlaylistFolder);

        downL->addWidget(new QLabel("Limite de downloads simultâneos:"));
        downL->addWidget(maxDown);
        downL->addWidget(new QLabel("Limite de Mbps por download (0 para ilimitado):"));
        downL->addWidget(speedLimit);
        downL->addWidget(playlistFolder);
        downL->addStretch();

        tabs->addTab(tabPadrao, "Padrão");
        tabs->addTab(tabDownload, "Download");
        mainL->addWidget(tabs);

        QHBoxLayout* buttons = new QHBoxLayout();
        QPushButton* saveBtn = new QPushButton("Salvar");
        saveBtn->setStyleSheet("background: #0078d7; color: white; padding: 6px;");
        QPushButton* cancelBtn = new QPushButton("Cancelar");
        buttons->addStretch();
        buttons->addWidget(saveBtn);
        buttons->addWidget(cancelBtn);
        mainL->addLayout(buttons);

        connect(browseBtn, &QPushButton::clicked, [&]() {
            QString dir = QFileDialog::getExistingDirectory(&diag, "Selecionar Pasta", pathEd->text());
            if (!dir.isEmpty()) pathEd->setText(dir);
        });

        connect(cancelBtn, &QPushButton::clicked, &diag, &QDialog::reject);
        connect(saveBtn, &QPushButton::clicked, [&]() {
            m_settings.defaultFormat = defFormat->currentText();
            m_settings.defaultQuality = defQual->currentText();
            m_settings.defaultAudioFormat = defAudioFormat->currentText();
            m_settings.defaultAudioQuality = defAudioQual->currentText();
            m_settings.downloadPath = pathEd->text();
            m_settings.maxConcurrent = maxDown->text().toInt();
            m_settings.speedLimit = speedLimit->text().replace(",", ".").toDouble();
            m_settings.createPlaylistFolder = playlistFolder->isChecked();
            saveSettings();
            diag.accept();
        });

        diag.exec();
    }

    void openAddDialog() {
        AddDownloadDialog diag(m_settings, this);
        if (diag.exec() == QDialog::Accepted) {
            addNewDownload(diag.getUrl(), diag.getPath(), diag.isVideo(), diag.getFormat(), diag.getQuality());
        }
    }

    void addNewDownload(QString url, QString path, bool isVideo, QString format, QString quality) {
        if(url.isEmpty()) return;

        QProcess* fetcher = new QProcess(this);
        QString binDir = QCoreApplication::applicationDirPath() + "/ffmpeg/";
        QString ytDlpPath = binDir + "yt-dlp.exe";

        #ifdef Q_OS_WIN
        fetcher->setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *args) {
            args->flags |= CREATE_NO_WINDOW;
        });
        #endif

        // Obtém JSON completo da playlist (flat para ser rápido)
        fetcher->start(ytDlpPath, {"--flat-playlist", "--dump-single-json", "--no-warnings", url});

        connect(fetcher, &QProcess::finished, [this, fetcher, path, isVideo, format, quality](int exitCode) {
            if (exitCode != 0) return;
            QJsonDocument doc = QJsonDocument::fromJson(fetcher->readAllStandardOutput());
            QJsonObject obj = doc.object();
            
            if (obj.contains("entries")) { // É uma Playlist
                QString plTitle = obj["title"].toString();
                QJsonArray entries = obj["entries"].toArray();
                
                QStringList urls;
                for(auto e : entries) urls << e.toObject()["url"].toString();

                PlaylistCard* pCard = new PlaylistCard(plTitle, urls);
                QListWidgetItem* pItem = new QListWidgetItem(downloadList);
                pItem->setSizeHint(pCard->sizeHint());
                downloadList->addItem(pItem);
                downloadList->setItemWidget(pItem, pCard);

                connect(pCard, &PlaylistCard::sizeChanged, [pItem, pCard]() {
                    pItem->setSizeHint(pCard->sizeHint());
                });

                // Carregar Thumbs para o Header usando um container seguro no heap
                auto headThumbs = std::make_shared<QList<QPixmap>>();
                int thumbTarget = qMin((int)entries.size(), 3);

                for(int i = 0; i < thumbTarget; ++i) {
                    QJsonArray thumbs = entries[i].toObject()["thumbnails"].toArray();
                    if (thumbs.isEmpty()) continue;

                    QString tUrl = thumbs.first().toObject()["url"].toString();
                    QNetworkReply* rep = m_netManager->get(QNetworkRequest(QUrl(tUrl)));
                    
                    // Capturamos 'headThumbs' por valor (shared_ptr) para garantir que a lista viva o suficiente
                    connect(rep, &QNetworkReply::finished, [this, rep, pCard, headThumbs, thumbTarget]() {
                        if (rep->error() == QNetworkReply::NoError) {
                            QPixmap p;
                            if (p.loadFromData(rep->readAll())) {
                                headThumbs->append(p);
                                if (headThumbs->size() >= thumbTarget) {
                                    pCard->setThumbs(*headThumbs);
                                }
                            }
                        }
                        rep->deleteLater();
                    });
                }

                for (auto e : entries) {
                    QJsonObject video = e.toObject();
                    DownloadCard* child = createDownloadCard(video["url"].toString(), path, isVideo, format, quality, true);
                    VideoMetadata meta;
                    meta.title = video["title"].toString();
                    meta.thumbnailUrl = video["thumbnails"].toArray().first().toObject()["url"].toString();
                    child->setInitialMetadata(meta);
                    pCard->addCard(child);
                }
            } else { // Vídeo único
                createDownloadCard(obj["webpage_url"].toString(), path, isVideo, format, quality, false);
            }
            fetcher->deleteLater();
        });
    }

    DownloadCard* createDownloadCard(QString url, QString path, bool isVideo, QString format, QString quality, bool isMini) {
        QListWidgetItem* item = new QListWidgetItem(downloadList);
        DownloadCard* card = new DownloadCard(url, isMini);
        item->setSizeHint(card->sizeHint());
        downloadList->addItem(item);
        downloadList->setItemWidget(item, card);
        downloadList->setCurrentItem(item); // Garante que o novo item seja selecionado
        
        connect(card, &DownloadCard::metadataLoaded, this, &MainWindow::onSelectionChanged);

        connect(card, &DownloadCard::clicked, this, [this, item](DownloadCard* clickedCard) {
            m_selectedCard = clickedCard;
            if (downloadList->currentItem() != item) {
                // Preserva a posição do scroll para evitar saltos para "partes vazias"
                int scrollPos = downloadList->verticalScrollBar()->value();
                downloadList->setCurrentItem(item);
                downloadList->verticalScrollBar()->setValue(scrollPos);
            }
            onSelectionChanged();
        });

        connect(card, &DownloadCard::requestRemoval, [this, item, card]() {
            // Remove da fila caso o download ainda não tenha começado
            m_queue.removeAll(card);
            m_paramsMap.remove(card);
            delete item;
        });
        
        // Adiciona à fila e salva os parâmetros para início posterior
        m_queue.append(card);
        m_paramsMap.insert(card, {path, isVideo, format, quality});
        
        processNextDownload();
        return card;
    }

    void processNextDownload() {
        const int MAX_CONCURRENT = 3; // Limite ideal para o seu Pentium G2030
        while (m_activeDownloads < MAX_CONCURRENT && !m_queue.isEmpty()) {
            DownloadCard* card = m_queue.takeFirst();
            if (!m_paramsMap.contains(card)) continue;

            DownloadParams params = m_paramsMap.take(card);
            m_activeDownloads++;

            // Quando o download terminar, libera o slot e chama o próximo da fila
            connect(card, &DownloadCard::finished, this, [this]() {
                m_activeDownloads--;
                processNextDownload();
            });

            card->startDownload(card->getUrl(), params.path, params.isVideo, params.format, params.quality);
        }
    }

    void onSelectionChanged() {
        QListWidgetItem* item = downloadList->currentItem();
        if (!item) return;

        QWidget* widget = downloadList->itemWidget(item);
        
        // Se o item selecionado na lista for uma playlist, tentamos pegar o card que o usuário clicou
        if (auto* p = qobject_cast<PlaylistCard*>(widget)) {
            if (!m_selectedCard || !p->getCards().contains(m_selectedCard)) {
                if (!p->getCards().isEmpty()) m_selectedCard = p->getCards().first();
            }
        } else if (auto* c = qobject_cast<DownloadCard*>(widget)) {
            m_selectedCard = c;
        }

        // Resetar estilos de todos os cards (globais e dentro de playlists)
        for(int i = 0; i < downloadList->count(); ++i) {
            QWidget* w = downloadList->itemWidget(downloadList->item(i));
            if(auto* c = qobject_cast<DownloadCard*>(w)) {
                c->setSelected(false);
            } else if(auto* p = qobject_cast<PlaylistCard*>(w)) {
                for(auto* child : p->getCards()) child->setSelected(false);
            }
        }

        if (m_selectedCard) {
            DownloadCard* selectedApp = m_selectedCard;
            selectedApp->setSelected(true);
            VideoMetadata meta = selectedApp->getMetadata();
            if (!meta.title.isEmpty()) {
                detailTitle->setText(meta.title);
                detailDesc->setText(meta.description.left(500) + "...");
                channelName->setText(meta.channel);
                
                // Atualiza thumbnail lateral
                QPixmap pix = selectedApp->getThumbnailPixmap();
                if (!pix.isNull()) {
                    detailThumb->setPixmap(pix.scaled(detailThumb->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation));
                } else {
                    detailThumb->setText("Carregando...");
                }

                // Carrega avatar do canal
                if (!meta.channelThumbUrl.isEmpty()) {
                    loadChannelAvatar(meta.channelThumbUrl);
                }
            }
        }
    }

    void loadSettings() {
        QString configPath = QCoreApplication::applicationDirPath() + "/config.json";
        QFile file(configPath);
        if (file.open(QIODevice::ReadOnly)) {
            QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
            QJsonObject obj = doc.object();
            m_settings.defaultFormat = obj["defaultFormat"].toString("MP4");
            m_settings.defaultQuality = obj["defaultQuality"].toString("1080p");
            m_settings.defaultAudioFormat = obj["defaultAudioFormat"].toString("MP3");
            m_settings.defaultAudioQuality = obj["defaultAudioQuality"].toString("128 kbps");
            m_settings.downloadPath = obj["downloadPath"].toString(QStandardPaths::writableLocation(QStandardPaths::DownloadLocation));
            m_settings.maxConcurrent = obj["maxConcurrent"].toInt(3);
            m_settings.speedLimit = obj["speedLimit"].toDouble(0.0);
            m_settings.createPlaylistFolder = obj["createPlaylistFolder"].toBool(true);
            file.close();
        }
    }

    void saveSettings() {
        QString configPath = QCoreApplication::applicationDirPath() + "/config.json";
        QFile file(configPath);
        if (file.open(QIODevice::WriteOnly)) {
            QJsonObject obj;
            obj["defaultFormat"] = m_settings.defaultFormat;
            obj["defaultQuality"] = m_settings.defaultQuality;
            obj["defaultAudioFormat"] = m_settings.defaultAudioFormat;
            obj["defaultAudioQuality"] = m_settings.defaultAudioQuality;
            obj["downloadPath"] = m_settings.downloadPath;
            obj["maxConcurrent"] = m_settings.maxConcurrent;
            obj["speedLimit"] = m_settings.speedLimit;
            obj["createPlaylistFolder"] = m_settings.createPlaylistFolder;
            file.write(QJsonDocument(obj).toJson());
            file.close();
        }
    }

private:
    QListWidget* downloadList;
    QLabel *detailThumb, *detailTitle, *channelPic, *channelName, *detailDesc;
    AppSettings m_settings;
    // Gerenciamento de Fila
    struct DownloadParams {
        QString path;
        bool isVideo;
        QString format;
        QString quality;
    };
    QList<DownloadCard*> m_queue;
    QMap<DownloadCard*, DownloadParams> m_paramsMap;
    int m_activeDownloads;
    QNetworkAccessManager* m_netManager;
    DownloadCard* m_selectedCard;
};

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    app.setStyle("Fusion"); // Estilo consistente em várias plataformas
    app.setWindowIcon(QIcon("icon.ico"));
    
    MainWindow w;
    w.show();
    
    return app.exec();
}

#include "Main.moc"
