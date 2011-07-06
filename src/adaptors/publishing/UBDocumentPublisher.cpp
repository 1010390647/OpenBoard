#include <QFileInfo>

#include "UBDocumentPublisher.h"

#include "frameworks/UBPlatformUtils.h"
#include "frameworks/UBFileSystemUtils.h"
#include "frameworks/UBStringUtils.h"

#include "network/UBNetworkAccessManager.h"
#include "network/UBServerXMLHttpRequest.h"

#include "core/UBDocumentManager.h"
#include "core/UBApplication.h"
#include "core/UBPersistenceManager.h"
#include "core/UBApplicationController.h"

#include "gui/UBMainWindow.h"

#include "document/UBDocumentProxy.h"

#include "domain/UBGraphicsWidgetItem.h"

#include "quazip.h"
#include "quazipfile.h"

#include "adaptors/UBExportFullPDF.h"
#include "adaptors/UBExportDocument.h"
#include "adaptors/UBSvgSubsetAdaptor.h"

#include "UBSvgSubsetRasterizer.h"

#include "core/memcheck.h"


UBDocumentPublisher::UBDocumentPublisher(UBDocumentProxy* pDocument, QObject *parent)
        : UBAbstractPublisher(parent)
        , mSourceDocument(pDocument)
        , mPublishingDocument(0)
        , mUsername("")
        , mPassword("")
		, bLoginCookieSet(false)
{
    mpWebView = new QWebView(0);
    UBApplication::mainWindow->addSankoreWebDocumentWidget(mpWebView);
    mpWebView->setWindowTitle(tr("Sankore Uploading Page"));
    mpWebView->setAcceptDrops(false);

    connect(mpWebView, SIGNAL(loadFinished(bool)), this, SLOT(onLoadFinished(bool)));
    connect(mpWebView, SIGNAL(linkClicked(QUrl)), this, SLOT(onLinkClicked(QUrl)));
    connect(this, SIGNAL(loginDone()), this, SLOT(onLoginDone()));


    init();
}


UBDocumentPublisher::~UBDocumentPublisher()
{
    //delete mpWebView;
    //delete mPublishingDocument;
}


void UBDocumentPublisher::publish()
{
    //check that the username and password are stored on preferences
    UBSettings* settings = UBSettings::settings();

    mUsername = settings->communityUsername();
    mPassword = settings->communityPassword();
    buildUbwFile();
    UBApplication::showMessage(tr("Uploading Sankore File on Web."));

    login(mUsername, mPassword);
    //sendUbw();

}

void UBDocumentPublisher::onLoginDone()
{
    sendUbw();
}

void UBDocumentPublisher::login(QString username, QString password)
{
    QString data,crlf;
    QByteArray datatoSend;

    // Create the request body
    data="srid=&j_username=" +username +"&j_password=" +password +crlf+crlf;
    datatoSend=data.toAscii(); // convert data string to byte array for request

    // Create the request header
    QString qsLoginURL = QString("http://sankore.devxwiki.com/xwiki/bin/loginsubmit/XWiki/XWikiLogin?xredirect=%0").arg(DOCPUBLICATION_URL);
    QNetworkRequest request(QUrl(qsLoginURL.toAscii().constData()));
    request.setRawHeader("Origin", "http://sankore.devxwiki.com");
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/x-www-form-urlencoded");
    request.setRawHeader("Accept", "application/xml,application/xhtml+xml,text/html;q=0.9,text/plain;q=0.8,image/png,*/*;q=0.5");
    request.setRawHeader("Referer", DOCPUBLICATION_URL);
    request.setHeader(QNetworkRequest::ContentLengthHeader,datatoSend.size());
    request.setRawHeader("Accept-Language", "en-US,*");

    // Generate a session id
    //mSessionID = getSessionID();

    // Create the cookie
    //QList<QNetworkCookie> cookiesList;
    //QString qsCookieValue;
    //qsCookieValue = mSessionID;
    //qsCookieValue += "; language=en";
    //QNetworkCookie cookie("JSESSIONID", qsCookieValue.toAscii().constData());
    //cookiesList << cookie;
    //request.setHeader(QNetworkRequest::CookieHeader, QVariant::fromValue(cookiesList));

    // Send the request
    mpNetworkMgr->post(request,datatoSend);
}



void UBDocumentPublisher::buildUbwFile()
{
    QDir d;
    d.mkpath(UBFileSystemUtils::defaultTempDirPath());

    QString tmpDir = UBFileSystemUtils::createTempDir();

    if (UBFileSystemUtils::copyDir(mSourceDocument->persistencePath(), tmpDir))
    {
        QUuid publishingUuid = QUuid::createUuid();

        mPublishingDocument = new UBDocumentProxy(tmpDir);
        mPublishingDocument->setPageCount(mSourceDocument->pageCount());

        rasterizeScenes();

        upgradeDocumentForPublishing();

        UBExportFullPDF pdfExporter;
        pdfExporter.setVerbode(false);
        pdfExporter.persistsDocument(mSourceDocument, mPublishingDocument->persistencePath() + "/" + UBStringUtils::toCanonicalUuid(publishingUuid) + ".pdf");

        UBExportDocument ubzExporter;
        ubzExporter.setVerbode(false);
        ubzExporter.persistsDocument(mSourceDocument, mPublishingDocument->persistencePath() + "/" + UBStringUtils::toCanonicalUuid(publishingUuid) + ".ubz");


        // remove all useless files

        for (int pageIndex = 0; pageIndex < mPublishingDocument->pageCount(); pageIndex++) {
            QString filename = mPublishingDocument->persistencePath() + UBFileSystemUtils::digitFileFormat("/page%1.svg", pageIndex + 1);

            QFile::remove(filename);
        }

        UBFileSystemUtils::deleteDir(mPublishingDocument->persistencePath() + "/" + UBPersistenceManager::imageDirectory);
        UBFileSystemUtils::deleteDir(mPublishingDocument->persistencePath() + "/" + UBPersistenceManager::objectDirectory);
        UBFileSystemUtils::deleteDir(mPublishingDocument->persistencePath() + "/" + UBPersistenceManager::videoDirectory);
        UBFileSystemUtils::deleteDir(mPublishingDocument->persistencePath() + "/" + UBPersistenceManager::audioDirectory);

        mTmpZipFile = UBFileSystemUtils::defaultTempDirPath() + "/" + UBStringUtils::toCanonicalUuid(QUuid::createUuid()) + ".zip";

        QuaZip zip(mTmpZipFile);
        zip.setFileNameCodec("UTF-8");
        if (!zip.open(QuaZip::mdCreate))
        {
            qWarning() << "Export failed. Cause: zip.open(): " << zip.getZipError() << "," << mTmpZipFile;
            QApplication::restoreOverrideCursor();
            return;
        }

        QuaZipFile outFile(&zip);

        if (!UBFileSystemUtils::compressDirInZip(mPublishingDocument->persistencePath(), "", &outFile, true))
        {
            qWarning("Export failed. compressDirInZip failed ...");
            zip.close();
            UBApplication::showMessage(tr("Export failed."));
            QApplication::restoreOverrideCursor();
            return;
        }

        if (zip.getZipError() != 0)
        {
            qWarning("Export failed. Cause: zip.close(): %d", zip.getZipError());
            zip.close();
            UBApplication::showMessage(tr("Export failed."));
            QApplication::restoreOverrideCursor();
            return;
        }

        zip.close();

    }
    else
    {
        UBApplication::showMessage(tr("Export canceled ..."));
        QApplication::restoreOverrideCursor();
    }
}

void UBDocumentPublisher::rasterizeScenes()
{

    for (int pageIndex = 0; pageIndex < mPublishingDocument->pageCount(); pageIndex++)
    {
        UBApplication::showMessage(tr("Converting page %1/%2 ...").arg(pageIndex + 1).arg(mPublishingDocument->pageCount()), true);

        UBSvgSubsetRasterizer rasterizer(mPublishingDocument, pageIndex);

        QString filename = mPublishingDocument->persistencePath() + UBFileSystemUtils::digitFileFormat("/page%1.jpg", pageIndex + 1);

        rasterizer.rasterizeToFile(filename);

    }
}


void UBDocumentPublisher::updateGoogleMapApiKey()
{
    QDir widgestDir(mPublishingDocument->persistencePath() + "/" + UBPersistenceManager::widgetDirectory);

    QString uniboardWebGoogleMapApiKey = UBSettings::settings()->uniboardWebGoogleMapApiKey->get().toString();

    foreach(QFileInfo dirInfo, widgestDir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot))
    {
        QString config = UBFileSystemUtils::readTextFile(dirInfo.absoluteFilePath() + "/config.xml").toLower();

        if (config.contains("google") && config.contains("map"))
        {
            QDir widgetDir(dirInfo.absoluteFilePath());

            foreach(QFileInfo fileInfo, widgetDir.entryInfoList(QDir::Files | QDir::NoDotAndDotDot))
            {
                QFile file(fileInfo.absoluteFilePath());

                if (file.open(QIODevice::ReadWrite))
                {
                    QTextStream stream(&file);
                    QString content = stream.readAll();

                    if (content.contains("ABQIAAAA6vtVqAUu8kZ_eTz7c8kwSBT9UCAhw_xm0LNFHsWmQxTJAdp5lxSY_5r-lZriY_7sACaMnl80JcX6Og"))
                    {
                        content.replace("ABQIAAAA6vtVqAUu8kZ_eTz7c8kwSBT9UCAhw_xm0LNFHsWmQxTJAdp5lxSY_5r-lZriY_7sACaMnl80JcX6Og",
                                        uniboardWebGoogleMapApiKey);

                        file.resize(0);
                        file.write(content.toUtf8());
                    }
                    file.close();
                }
            }
        }
    }
}


void UBDocumentPublisher::upgradeDocumentForPublishing()
{
    for (int pageIndex = 0; pageIndex < mPublishingDocument->pageCount(); pageIndex++)
    {
        UBGraphicsScene *scene = UBSvgSubsetAdaptor::loadScene(mPublishingDocument, pageIndex);

        bool sceneHasWidget = false;

        QList<UBGraphicsW3CWidgetItem*> widgets;

        foreach(QGraphicsItem* item, scene->items()){
            UBGraphicsW3CWidgetItem *widgetItem = dynamic_cast<UBGraphicsW3CWidgetItem*>(item);

            if(widgetItem){
                generateWidgetPropertyScript(widgetItem, pageIndex + 1);
                sceneHasWidget = true;
                widgets << widgetItem;
            }
        }

        QString filename = mPublishingDocument->persistencePath() + UBFileSystemUtils::digitFileFormat("/page%1.json", pageIndex + 1);

        QFile jsonFile(filename);
        if (jsonFile.open(QIODevice::WriteOnly | QIODevice::Truncate))
        {
            jsonFile.write("{\n");
            jsonFile.write(QString("  \"scene\": {\n").toUtf8());
            jsonFile.write(QString("    \"x\": %1,\n").arg(scene->normalizedSceneRect().x()).toUtf8());
            jsonFile.write(QString("    \"y\": %1,\n").arg(scene->normalizedSceneRect().y()).toUtf8());
            jsonFile.write(QString("    \"width\": %1,\n").arg(scene->normalizedSceneRect().width()).toUtf8());
            jsonFile.write(QString("    \"height\": %1\n").arg(scene->normalizedSceneRect().height()).toUtf8());
            jsonFile.write(QString("  },\n").toUtf8());

            jsonFile.write(QString("  \"widgets\": [\n").toUtf8());

            bool first = true;

            foreach(UBGraphicsW3CWidgetItem* widget, widgets)
            {
                if (!first)
                    jsonFile.write(QString("    ,\n").toUtf8());

                jsonFile.write(QString("    {\n").toUtf8());
                jsonFile.write(QString("      \"uuid\": \"%1\",\n").arg(UBStringUtils::toCanonicalUuid(widget->uuid())).toUtf8());
                jsonFile.write(QString("      \"id\": \"%1\",\n").arg(widget->metadatas().id).toUtf8());

                jsonFile.write(QString("      \"name\": \"%1\",\n").arg(widget->w3cWidget()->metadatas().name).toUtf8());
                jsonFile.write(QString("      \"description\": \"%1\",\n").arg(widget->w3cWidget()->metadatas().description).toUtf8());
                jsonFile.write(QString("      \"author\": \"%1\",\n").arg(widget->w3cWidget()->metadatas().author).toUtf8());
                jsonFile.write(QString("      \"authorEmail\": \"%1\",\n").arg(widget->w3cWidget()->metadatas().authorEmail).toUtf8());
                jsonFile.write(QString("      \"authorHref\": \"%1\",\n").arg(widget->w3cWidget()->metadatas().authorHref).toUtf8());
                jsonFile.write(QString("      \"version\": \"%1\",\n").arg(widget->w3cWidget()->metadatas().authorHref).toUtf8());

                jsonFile.write(QString("      \"x\": %1,\n").arg(widget->sceneBoundingRect().x()).toUtf8());
                jsonFile.write(QString("      \"y\": %1,\n").arg(widget->sceneBoundingRect().y()).toUtf8());
                jsonFile.write(QString("      \"width\": %1,\n").arg(widget->sceneBoundingRect().width()).toUtf8());
                jsonFile.write(QString("      \"height\": %1,\n").arg(widget->sceneBoundingRect().height()).toUtf8());

                jsonFile.write(QString("      \"nominalWidth\": %1,\n").arg(widget->boundingRect().width()).toUtf8());
                jsonFile.write(QString("      \"nominalHeight\": %1,\n").arg(widget->boundingRect().height()).toUtf8());

                QString url = UBPersistenceManager::widgetDirectory + "/" + widget->uuid().toString() + ".wgt";
                jsonFile.write(QString("      \"src\": \"%1\",\n").arg(url).toUtf8());
                QString startFile = widget->w3cWidget()->mainHtmlFileName();
                jsonFile.write(QString("      \"startFile\": \"%1\",\n").arg(startFile).toUtf8());

                QMap<QString, QString> preferences = widget->preferences();

                jsonFile.write(QString("      \"preferences\": {\n").toUtf8());

                foreach(QString key, preferences.keys())
                {
                    QString sep = ",";
                    if (key == preferences.keys().last())
                        sep = "";

                    jsonFile.write(QString("          \"%1\": \"%2\"%3\n")
                                   .arg(key)
                                   .arg(preferences.value(key))
                                   .arg(sep)
                                   .toUtf8());
                }
                jsonFile.write(QString("      },\n").toUtf8());

                jsonFile.write(QString("      \"datastore\": {\n").toUtf8());

                QMap<QString, QString> datastoreEntries = widget->datastoreEntries();

                foreach(QString entry, datastoreEntries.keys())
                {
                    QString sep = ",";
                    if (entry == datastoreEntries.keys().last())
                        sep = "";

                    jsonFile.write(QString("          \"%1\": \"%2\"%3\n")
                                   .arg(entry)
                                   .arg(datastoreEntries.value(entry))
                                   .arg(sep)
                                   .toUtf8());
                }
                jsonFile.write(QString("      }\n").toUtf8());

                jsonFile.write(QString("    }\n").toUtf8());

                first = false;
            }

            jsonFile.write("  ]\n");
            jsonFile.write("}\n");
        }
        else
        {
            qWarning() << "Cannot open file" << filename << "for saving page state";
        }

        delete scene;
    }

    updateGoogleMapApiKey();
}


void UBDocumentPublisher::generateWidgetPropertyScript(UBGraphicsW3CWidgetItem *widgetItem, int pageNumber)
{

    QMap<QString, QString> preferences = widgetItem->preferences();
    QMap<QString, QString> datastoreEntries = widgetItem->datastoreEntries();

    QString startFileName = widgetItem->w3cWidget()->mainHtmlFileName();

    if (!startFileName.startsWith("http://"))
    {
        QString startFilePath = mPublishingDocument->persistencePath() + "/" + UBPersistenceManager::widgetDirectory + "/" + widgetItem->uuid().toString() + ".wgt/" + startFileName;

        QFile startFile(startFilePath);

        if (startFile.exists())
        {
            if (startFile.open(QIODevice::ReadWrite))
            {
                QTextStream stream(&startFile);
                QStringList lines;

                bool addedJs = false;

                QString line;
                do
                {
                    line = stream.readLine();
                    if (!line.isNull())
                    {
                        lines << line;

                        if (!addedJs && line.contains("<head") && line.contains(">") )  // TODO UB 4.6, this is naive ... the HEAD tag may be on several lines
                        {
                            lines << "";
                            lines << "  <script type=\"text/javascript\">";

                            lines << "    var widget = {};";
                            lines << "    widget.id = '" + widgetItem->w3cWidget()->metadatas().id + "';";
                            lines << "    widget.name = '" + widgetItem->w3cWidget()->metadatas().name + "';";
                            lines << "    widget.description = '" + widgetItem->w3cWidget()->metadatas().description + "';";
                            lines << "    widget.author = '" + widgetItem->w3cWidget()->metadatas().author + "';";
                            lines << "    widget.authorEmail = '" + widgetItem->w3cWidget()->metadatas().authorEmail + "';";
                            lines << "    widget.authorHref = '" + widgetItem->w3cWidget()->metadatas().authorHref + "';";
                            lines << "    widget.version = '" + widgetItem->w3cWidget()->metadatas().version + "';";

                            lines << "    widget.uuid = '" + UBStringUtils::toCanonicalUuid(widgetItem->uuid()) + "';";

                            lines << "    widget.width = " + QString("%1").arg(widgetItem->w3cWidget()->width()) + ";";
                            lines << "    widget.height = " + QString("%1").arg(widgetItem->w3cWidget()->height()) + ";";
                            lines << "    widget.openUrl = function(url) { window.open(url); }";
                            lines << "    widget.preferences = new Array()";

                            foreach(QString pref, preferences.keys())
                            {
                                lines << "      widget.preferences['" + pref + "'] = '" + preferences.value(pref) + "';";
                            }

                            lines << "    widget.preferences.key = function(index) {";
                            lines << "      var currentIndex = 0;";
                            lines << "      for(key in widget.preferences){";
                            lines << "        if (currentIndex == index){ return key;}";
                            lines << "        currentIndex++;";
                            lines << "      }";
                            lines << "      return '';";
                            lines << "    }";

                            lines << "    widget.preferences.getItem = function(key) {";
                            lines << "      return widget.preferences[key];";
                            lines << "    }";

                            lines << "    widget.preferences.setItem = function(key, value) {}";
                            lines << "    widget.preferences.removeItem = function(key) {}";
                            lines << "    widget.preferences.clear = function() {}";

                            lines << "    var uniboard = {};";
                            lines << "    uniboard.pageCount = " + QString("%1").arg(mPublishingDocument->pageCount()) + ";";
                            lines << "    uniboard.currentPageNumber = " + QString("%1").arg(pageNumber) + ";";
                            lines << "    uniboard.uuid = '" + UBStringUtils::toCanonicalUuid(widgetItem->uuid()) + "'";
                            lines << "    uniboard.lang = navigator.language;";
                            lines << "    uniboard.locale = function() {return navigator.language}";
                            lines << "    uniboard.messages = {}";
                            lines << "    uniboard.messages.subscribeToTopic = function(topicName){}";
                            lines << "    uniboard.messages.unsubscribeFromTopic = function(topicName){}";
                            lines << "    uniboard.messages.sendMessage = function(topicName, message){}";

                            lines << "    uniboard.datastore = {};";
                            lines << "    uniboard.datastore.document = new Array();";
                            foreach(QString entry, datastoreEntries.keys())
                            {
                                lines << "      uniboard.datastore.document['" + entry + "'] = '" + datastoreEntries.value(entry) + "';";
                            }

                            lines << "    uniboard.datastore.document.key = function(index) {";
                            lines << "      var currentIndex = 0;";
                            lines << "      for(key in uniboard.datastore.document){";
                            lines << "        if (currentIndex == index){ return key;}";
                            lines << "        currentIndex++;";
                            lines << "      }";
                            lines << "      return '';";
                            lines << "    }";

                            lines << "    uniboard.datastore.document.getItem = function(key) {";
                            lines << "      return uniboard.datastore.document[key];";
                            lines << "    }";

                            lines << "    uniboard.datastore.document.setItem = function(key, value) {}";
                            lines << "    uniboard.datastore.document.removeItem = function(key) {}";
                            lines << "    uniboard.datastore.document.clear = function() {}";

                            lines << "    uniboard.setTool = function(tool){}";
                            lines << "    uniboard.setPenColor = function(color){}";
                            lines << "    uniboard.setMarkerColor = function(color){}";

                            lines << "    uniboard.pageThumbnail = function(pageNumber){";
                            lines << "      var nb;";
                            lines << "      if (pageNumber < 10) return 'page00' + pageNumber + '.thumbnail.jpg';";
                            lines << "      if (pageNumber < 100) return 'page0' + pageNumber + '.thumbnail.jpg';";
                            lines << "      return 'page' + pageNumber + '.thumbnail.jpg;'";
                            lines << "    }";

                            lines << "    uniboard.zoom = function(factor, x, y){}";
                            lines << "    uniboard.move = function(x, y){}";
                            lines << "    uniboard.move = function(x, y){}";
                            lines << "    uniboard.moveTo = function(x, y){}";
                            lines << "    uniboard.drawLineTo = function(x, y, width){}";
                            lines << "    uniboard.eraseLineTo = function(x, y, width){}";
                            lines << "    uniboard.clear = function(){}";
                            lines << "    uniboard.setBackground = function(dark, crossed){}";
                            lines << "    uniboard.addObject = function(url, width, height, x, y, background){}";
                            lines << "    uniboard.resize = function(width, height){window.resizeTo(width, height);}";

                            lines << "    uniboard.showMessage = function(message){alert(message);}";
                            lines << "    uniboard.centerOn = function(x, y){}";
                            lines << "    uniboard.addText = function(text, x, y){}";

                            lines << "    uniboard.setPreference = function(key, value){}";
                            lines << "    uniboard.preference = function(key, defValue){";
                            lines << "      var pref = widget.preferences[key];";
                            lines << "      if (pref == undefined) ";
                            lines << "        return defValue;";
                            lines << "      else ";
                            lines << "        return pref;";
                            lines << "    }";
                            lines << "    uniboard.preferenceKeys = function(){";
                            lines << "        var keys = new Array();";
                            lines << "        for(key in widget.preferences){";
                            lines << "            keys.push(key);";
                            lines << "        }";
                            lines << "        return keys;";
                            lines << "    }";

                            lines << "    uniboard.datastore.document.key = function(index) {";
                            lines << "        var currentIndex = 0;";
                            lines << "        for(key in uniboard.datastore.document){";
                            lines << "            if (currentIndex == index){ return key;}";
                            lines << "            currentIndex++;";
                            lines << "        }";
                            lines << "        return '';";
                            lines << "    }";

                            lines << "    uniboard.datastore.document.getItem = function(key) {";
                            lines << "        return uniboard.datastore.document[key];";
                            lines << "    }";

                            lines << "    uniboard.datastore.document.setItem = function(key, value) {}";
                            lines << "    uniboard.datastore.document.removeItem = function(key) {}";
                            lines << "    uniboard.datastore.document.clear = function() {}";

                            lines << "  </script>";
                            lines << "";

                            addedJs = true;
                        }
                    }
                }
                while (!line.isNull());

                startFile.resize(0);
                startFile.write(lines.join("\n").toUtf8()); // TODO UB 4.x detect real html encoding

                startFile.close();
            }
        }
    }
    else{
        qWarning() << "Remote Widget start file, cannot inject widget preferences and datastore entries";
    }
}




void UBDocumentPublisher::init()
{
    mCrlf=0x0d;
    mCrlf+=0x0a;

    mpNetworkMgr = new QNetworkAccessManager(this);
    //mpCache = new QNetworkDiskCache(this);
    //mpCache->setCacheDirectory("cache");
    //mpNetworkMgr->setCache(mpCache);
    mpCookieJar = new QNetworkCookieJar();

    connect(mpNetworkMgr, SIGNAL(finished(QNetworkReply*)), this, SLOT(onFinished(QNetworkReply*)));
}

void UBDocumentPublisher::onFinished(QNetworkReply *reply)
{
    qDebug() << "[-[ Request finished! ]-]";
    QByteArray response = reply->readAll();

    if (!bLoginCookieSet)
    {
        QVariant cookieHeader = reply->rawHeader("Set-Cookie");
        // First we concatenate all the Set-Cookie values (the packet can contains many of them)
        QStringList qslCookie = cookieHeader.toString().split("\n");
        QString qsCookieValue = qslCookie.at(0);
        for (int i = 1; i < qslCookie.size(); i++) {
            qsCookieValue += "; " +qslCookie.at(i);
        }

        // Now we isolate every cookie value
        QStringList qslCookieVals = qsCookieValue.split("; ");

        // Finally we create the cookies
        for (int i = 0; i < qslCookieVals.size(); i++)
        {
            QString cookieString = qslCookieVals.at(i);
            //qDebug() << "qslCookieVals.at(i): " << cookieString.replace("\"", "");
            QStringList qslCrntCookie = cookieString.split("=");
            QNetworkCookie crntCookie;
            if (qslCrntCookie.length() == 2)
            {
                QString qsValue = qslCrntCookie.at(1);
                qsValue.remove("\"");
                crntCookie = QNetworkCookie(qslCrntCookie.at(0).toAscii().constData(), qsValue.toAscii().constData());
            }
            else
            {
                crntCookie = QNetworkCookie(qslCrntCookie.at(0).toAscii().constData());
            }
            // HACK : keep only the same cookies as the XWiki website does.
            if(crntCookie.name() == "JSESSIONID" ||
               crntCookie.name() == "username" ||
               crntCookie.name() == "password" ||
               crntCookie.name() == "rememberme" ||
               crntCookie.name() == "validation")
            {
                mCookies << crntCookie;
            }
        }
        QNetworkCookie langCookie("language", "en");
        mCookies << langCookie;
        // DEBUG : Verify
        for(int i = 0; i < mCookies.size(); i++)
        {
            qDebug() << mCookies.at(i).name() << "=" << mCookies.at(i).value();
        }

        // Set the cookiejar : it set the cookies that will be sent with every packet.
        mpCookieJar->setCookiesFromUrl(mCookies, QUrl(DOCPUBLICATION_URL)/*reply->url()*/);

        mpNetworkMgr->setCookieJar(mpCookieJar);
        bLoginCookieSet = true;
        emit loginDone();
    }
    else
    {
        if (!response.isEmpty()){
            // Display the iframe
            mpWebView->setHtml(response, reply->url());
            UBApplication::applicationController->showSankoreWebDocument();
        }
        else
        {
            // Redirect
            QVariant locationHeader = reply->rawHeader("Location");

            QNetworkRequest req(QUrl(locationHeader.toString()));
            mpNetworkMgr->get(req);
            qDebug() << mpWebView->url().toString();
        }
    }
    reply->deleteLater();
}

void UBDocumentPublisher::sendUbw()
{
    if (QFile::exists(mTmpZipFile))
    {
        QFile f(mTmpZipFile);
        if (f.open(QIODevice::ReadOnly))
        {
            QFileInfo fi(f);
            QByteArray ba = f.readAll();
            QString boundary,data, multipartHeader;
            QByteArray datatoSend;

            boundary = "---WebKitFormBoundaryDKBTgA53MiyWrzLY";
            multipartHeader = "multipart/form-data; boundary="+boundary;

            data="--"+boundary+mCrlf;
            data+="Content-Disposition: form-data; name=\"file\"; filename=\""+ fi.fileName() +"\""+mCrlf;
            data+="Content-Type: application/octet-stream"+mCrlf+mCrlf;
            datatoSend=data.toAscii(); // convert data string to byte array for request
            datatoSend += ba;
            datatoSend += mCrlf;
            datatoSend += QString("--%0--%1").arg(boundary).arg(mCrlf);

            QNetworkRequest request(QUrl(DOCPUBLICATION_URL));
            request.setHeader(QNetworkRequest::ContentTypeHeader, multipartHeader);
            request.setHeader(QNetworkRequest::ContentLengthHeader,datatoSend.size());
            request.setRawHeader("Accept", "application/xml,application/xhtml+xml,text/html;q=0.9,text/plain;q=0.8,image/png,*/*;q=0.5");
            request.setRawHeader("Accept-Language", "en-US,*");
            request.setRawHeader("Referer", DOCPUBLICATION_URL);

            // Send the file
            mpNetworkMgr->post(request,datatoSend);
        }
    }
}

QString UBDocumentPublisher::getBase64Of(QString stringToEncode)
{
    return stringToEncode.toAscii().toBase64();
}

void UBDocumentPublisher::onLinkClicked(const QUrl &url)
{
    // [Basic Auth] Here we interpret the link and send the request with the basic auth header.
    QNetworkRequest request;
    request.setUrl(url);
    QString b64Auth = getBase64Of(QString("%0:%1").arg(mUsername).arg(mPassword));
    request.setRawHeader("Authorization", QString("Basic %0").arg(b64Auth).toAscii().constData());
    mpNetworkMgr->get(request);
}

void UBDocumentPublisher::onLoadFinished(bool result)
{
    Q_UNUSED(result);
    // [Basic Auth] This line says: if the user click on a link, do not interpret it.
    //mpWebView->page()->setLinkDelegationPolicy(QWebPage::DelegateAllLinks);
    mpWebView->page()->setNetworkAccessManager(mpNetworkMgr);
}





