/*
 * Copyright (C) by Olivier Goffart <ogoffart@woboq.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
 * for more details.
 */

#include "discoveryphase.h"
#include <csync_private.h>
#include <qdebug.h>

#include <QUrl>
#include "account.h"
#include <QFileInfo>

namespace OCC {

bool DiscoveryJob::isInSelectiveSyncBlackList(const QString& path) const
{
    if (_selectiveSyncBlackList.isEmpty()) {
        // If there is no black list, everything is allowed
        return false;
    }

    // If one of the item in the black list is a prefix of the path, it means this path need not to
    // be synced.
    //
    // We know the list is sorted (for it is done in DiscoveryJob::start)
    // So we can do a binary search. If the path is a prefix if another item or right after in the lexical order.

    QString pathSlash = path + QLatin1Char('/');

    auto it = std::lower_bound(_selectiveSyncBlackList.begin(), _selectiveSyncBlackList.end(), pathSlash);

    if (it != _selectiveSyncBlackList.end() && *it == pathSlash) {
        return true;
    }

	if (it == _selectiveSyncBlackList.begin()) {
        return false;
    }
    --it;
    Q_ASSERT(it->endsWith(QLatin1Char('/'))); // Folder::setSelectiveSyncBlackList makes sure of that
    if (pathSlash.startsWith(*it)) {
        return true;
    }
    return false;
}

int DiscoveryJob::isInSelectiveSyncBlackListCallBack(void *data, const char *path)
{
    return static_cast<DiscoveryJob*>(data)->isInSelectiveSyncBlackList(QString::fromUtf8(path));
}

void DiscoveryJob::update_job_update_callback (bool local,
                                    const char *dirUrl,
                                    void *userdata)
{
    DiscoveryJob *updateJob = static_cast<DiscoveryJob*>(userdata);
    if (updateJob) {
        // Don't wanna overload the UI
        if (!updateJob->lastUpdateProgressCallbackCall.isValid()) {
            updateJob->lastUpdateProgressCallbackCall.restart(); // first call
        } else if (updateJob->lastUpdateProgressCallbackCall.elapsed() < 200) {
            return;
        } else {
            updateJob->lastUpdateProgressCallbackCall.restart();
        }

        QString path(QUrl::fromPercentEncoding(QByteArray(dirUrl)).section('/', -1));
        emit updateJob->folderDiscovered(local, path);
    }
}


int get_errno_from_http_errcode( int err ) {
    int new_errno = 0;

    switch(err) {
    case 200:           /* OK */
    case 201:           /* Created */
    case 202:           /* Accepted */
    case 203:           /* Non-Authoritative Information */
    case 204:           /* No Content */
    case 205:           /* Reset Content */
    case 207:           /* Multi-Status */
    case 304:           /* Not Modified */
        new_errno = 0;
        break;
    case 401:           /* Unauthorized */
    case 402:           /* Payment Required */
    case 407:           /* Proxy Authentication Required */
    case 405:
        new_errno = EPERM;
        break;
    case 301:           /* Moved Permanently */
    case 303:           /* See Other */
    case 404:           /* Not Found */
    case 410:           /* Gone */
        new_errno = ENOENT;
        break;
    case 408:           /* Request Timeout */
    case 504:           /* Gateway Timeout */
        new_errno = EAGAIN;
        break;
    case 423:           /* Locked */
        new_errno = EACCES;
        break;
    case 400:           /* Bad Request */
    case 403:           /* Forbidden */
    case 409:           /* Conflict */
    case 411:           /* Length Required */
    case 412:           /* Precondition Failed */
    case 414:           /* Request-URI Too Long */
    case 415:           /* Unsupported Media Type */
    case 424:           /* Failed Dependency */
    case 501:           /* Not Implemented */
        new_errno = EINVAL;
        break;
    case 507:           /* Insufficient Storage */
        new_errno = ENOSPC;
        break;
    case 206:           /* Partial Content */
    case 300:           /* Multiple Choices */
    case 302:           /* Found */
    case 305:           /* Use Proxy */
    case 306:           /* (Unused) */
    case 307:           /* Temporary Redirect */
    case 406:           /* Not Acceptable */
    case 416:           /* Requested Range Not Satisfiable */
    case 417:           /* Expectation Failed */
    case 422:           /* Unprocessable Entity */
    case 500:           /* Internal Server Error */
    case 502:           /* Bad Gateway */
    case 505:           /* HTTP Version Not Supported */
        new_errno = EIO;
        break;
    case 503:           /* Service Unavailable */
        new_errno = ERRNO_SERVICE_UNAVAILABLE;
        // FIXME Distinguish between service unavailable and storage unavilable
        break;
    case 413:           /* Request Entity too Large */
        new_errno = EFBIG;
        break;
    default:
        new_errno = EIO;
    }
    return new_errno;
}



DiscoverySingleDirectoryJob::DiscoverySingleDirectoryJob(AccountPtr account, const QString &path, QObject *parent)
    : QObject(parent), _subPath(path), _account(account), _ignoredFirst(false)
{
}

void DiscoverySingleDirectoryJob::start()
{
    // Start the actual HTTP job
    LsColJob *lsColJob = new LsColJob(_account, _subPath, this);
    QObject::connect(lsColJob, SIGNAL(directoryListingIterated(QString,QMap<QString,QString>)),
                     this, SLOT(directoryListingIteratedSlot(QString,QMap<QString,QString>)));
    QObject::connect(lsColJob, SIGNAL(finishedWithError(QNetworkReply*)), this, SLOT(lsJobFinishedWithErrorSlot(QNetworkReply*)));
    QObject::connect(lsColJob, SIGNAL(finishedWithoutError()), this, SLOT(lsJobFinishedWithoutErrorSlot()));
    lsColJob->start();

    _lsColJob = lsColJob;
}

void DiscoverySingleDirectoryJob::abort()
{
    if (_lsColJob && _lsColJob->reply()) {
        _lsColJob->reply()->abort();
    }
}

static csync_vio_file_stat_t* propertyMapToFileStat(QMap<QString,QString> map)
{
    csync_vio_file_stat_t* file_stat = csync_vio_file_stat_new();

    for (auto it = map.constBegin(); it != map.constEnd(); ++it) {
        //qDebug() << it.key() << it.value();
        QString property = it.key();
        QString value = it.value();
        if (property == "resourcetype") {
            if (value.contains("collection")) {
                file_stat->type = CSYNC_VIO_FILE_TYPE_DIRECTORY;
            } else {
                file_stat->type = CSYNC_VIO_FILE_TYPE_REGULAR;
            }
            file_stat->fields |= CSYNC_VIO_FILE_STAT_FIELDS_TYPE;
        } else if  (property == "getlastmodified") {
            file_stat->mtime = oc_httpdate_parse(value.toUtf8());
            file_stat->fields |= CSYNC_VIO_FILE_STAT_FIELDS_MTIME;
        } else if (property == "getcontentlength") {
            file_stat->size = value.toLongLong();
            file_stat->fields |= CSYNC_VIO_FILE_STAT_FIELDS_SIZE;
        } else if (property == "getetag") {
            file_stat->etag = csync_normalize_etag(value.toUtf8());
            file_stat->fields |= CSYNC_VIO_FILE_STAT_FIELDS_ETAG;
        } else if (property == "id") {
            csync_vio_file_stat_set_file_id(file_stat, value.toUtf8());
        } else if (property == "downloadURL") {
            file_stat->directDownloadUrl = strdup(value.toUtf8());
            file_stat->fields |= CSYNC_VIO_FILE_STAT_FIELDS_DIRECTDOWNLOADURL;
        } else if (property == "dDC") {
            file_stat->directDownloadCookies = strdup(value.toUtf8());
            file_stat->fields |= CSYNC_VIO_FILE_STAT_FIELDS_DIRECTDOWNLOADCOOKIES;
        } else if (property == "permissions") {
            if (value.isEmpty()) {
                // special meaning for our code: server returned permissions but are empty
                // meaning only reading is allowed for this resource
                file_stat->remotePerm[0] = ' ';
                // see _csync_detect_update()
                file_stat->fields |= CSYNC_VIO_FILE_STAT_FIELDS_PERM;
            } else if (value.length() < int(sizeof(file_stat->remotePerm))) {
                strncpy(file_stat->remotePerm, value.toUtf8(), sizeof(file_stat->remotePerm));
                file_stat->fields |= CSYNC_VIO_FILE_STAT_FIELDS_PERM;
            } else {
                // old server, keep file_stat->remotePerm empty
            }
        }
    }

    return file_stat;
}

void DiscoverySingleDirectoryJob::directoryListingIteratedSlot(QString file,QMap<QString,QString> map)
{
    //qDebug() << Q_FUNC_INFO << _subPath << file << map.count() << map.keys() << _account->davPath() << _lsColJob->reply()->request().url().path();
    if (!_ignoredFirst) {
        // First result is the directory itself. Maybe should have a better check for that? FIXME
        _ignoredFirst = true;
        if (map.contains("permissions")) {
            emit firstDirectoryPermissions(map.value("permissions"));
        }
        if (map.contains("getetag")) {
            emit firstDirectoryEtag(map.value("getetag"));
        }
    } else {
        // Remove <webDAV-Url>/folder/ from <webDAV-Url>/folder/subfile.txt
        file.remove(0, _lsColJob->reply()->request().url().path().length());
        // remove trailing slash
        while (file.endsWith('/')) {
            file.chop(1);
        }
        // remove leading slash
        while (file.startsWith('/')) {
            file = file.remove(0, 1);
        }


        csync_vio_file_stat_t *file_stat = propertyMapToFileStat(map);
        file_stat->name = strdup(file.toUtf8());
        //qDebug() << "!!!!" << file_stat << file_stat->name << file_stat->file_id << map.count();
        _results.append(file_stat);
    }

}

void DiscoverySingleDirectoryJob::lsJobFinishedWithoutErrorSlot()
{
    emit finishedWithResult(_results);
    deleteLater();
}

void DiscoverySingleDirectoryJob::lsJobFinishedWithErrorSlot(QNetworkReply *r)
{
    QString contentType = r->header(QNetworkRequest::ContentTypeHeader).toString();
    int httpCode = r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    QString msg = r->errorString();
    int errnoCode = 0;
    qDebug() << Q_FUNC_INFO << r->errorString() << httpCode << r->error();
    if (r->error() != QNetworkReply::NoError) {
        errnoCode = EIO;
    } else if (httpCode != 207) {
        errnoCode = get_errno_from_http_errcode(httpCode);
    } else if (!contentType.contains("application/xml; charset=utf-8")) {
        msg = QLatin1String("Server error: PROPFIND reply is not XML formatted!");
        errnoCode = ERRNO_WRONG_CONTENT;
    }

    emit finishedWithError(errnoCode, msg);
    deleteLater();
}

void DiscoveryMainThread::setupHooks(DiscoveryJob *discoveryJob, const QString &pathPrefix)
{
    _discoveryJob = discoveryJob;
    _pathPrefix = pathPrefix;

    connect(discoveryJob, SIGNAL(doOpendirSignal(QString,DiscoveryDirectoryResult*)),
            this, SLOT(doOpendirSlot(QString,DiscoveryDirectoryResult*)),
            Qt::QueuedConnection);
}

// Coming from owncloud_opendir -> DiscoveryJob::vio_opendir_hook -> doOpendirSignal
void DiscoveryMainThread::doOpendirSlot(QString subPath, DiscoveryDirectoryResult *r)
{
    QString fullPath = _pathPrefix;
    if (!_pathPrefix.endsWith('/')) {
        fullPath += '/';
    }
    fullPath += subPath;
    // remove trailing slash
    while (fullPath.endsWith('/')) {
        fullPath.chop(1);
    }
    qDebug() << Q_FUNC_INFO << _pathPrefix << subPath << fullPath;


    // Result gets written in there
    _currentDiscoveryDirectoryResult = r;
    _currentDiscoveryDirectoryResult->path = fullPath;

    // Schedule the DiscoverySingleDirectoryJob
    _singleDirJob = new DiscoverySingleDirectoryJob(_account, fullPath, this);
    QObject::connect(_singleDirJob, SIGNAL(finishedWithResult(QLinkedList<csync_vio_file_stat_t *>)),
                     this, SLOT(singleDirectoryJobResultSlot(QLinkedList<csync_vio_file_stat_t*>)));
    QObject::connect(_singleDirJob, SIGNAL(finishedWithError(int,QString)),
                     this, SLOT(singleDirectoryJobFinishedWithErrorSlot(int,QString)));
    QObject::connect(_singleDirJob, SIGNAL(firstDirectoryPermissions(QString)),
                     this, SLOT(singleDirectoryJobFirstDirectoryPermissionsSlot(QString)));
    QObject::connect(_singleDirJob, SIGNAL(firstDirectoryEtag(QString)),
                     this, SIGNAL(rootEtag(QString)));
    _singleDirJob->start();
}


void DiscoveryMainThread::singleDirectoryJobResultSlot(QLinkedList<csync_vio_file_stat_t *> result)
{
    if (!_currentDiscoveryDirectoryResult) {
        return; // possibly aborted
    }
    qDebug() << Q_FUNC_INFO << "Have" << result.count() << "results for " << _currentDiscoveryDirectoryResult->path;


    _directoryContents.insert(_currentDiscoveryDirectoryResult->path, result);

    _currentDiscoveryDirectoryResult->list = result;
    _currentDiscoveryDirectoryResult->code = 0;
    _currentDiscoveryDirectoryResult->iterator = _currentDiscoveryDirectoryResult->list.begin();
     _currentDiscoveryDirectoryResult = 0; // the sync thread owns it now

    _discoveryJob->_vioMutex.lock();
    _discoveryJob->_vioWaitCondition.wakeAll();
    _discoveryJob->_vioMutex.unlock();
}

void DiscoveryMainThread::singleDirectoryJobFinishedWithErrorSlot(int csyncErrnoCode, QString msg)
{
    if (!_currentDiscoveryDirectoryResult) {
        return; // possibly aborted
    }
    qDebug() << Q_FUNC_INFO;

     _currentDiscoveryDirectoryResult->code = csyncErrnoCode;
     _currentDiscoveryDirectoryResult->msg = msg;
     _currentDiscoveryDirectoryResult = 0; // the sync thread owns it now

    _discoveryJob->_vioMutex.lock();
    _discoveryJob->_vioWaitCondition.wakeAll();
    _discoveryJob->_vioMutex.unlock();
}

void DiscoveryMainThread::singleDirectoryJobFirstDirectoryPermissionsSlot(QString p)
{
    // Should be thread safe since the sync thread is blocked
    if (!_discoveryJob->_csync_ctx->remote.root_perms) {
        qDebug() << "Permissions for root dir:" << p;
        _discoveryJob->_csync_ctx->remote.root_perms = strdup(p.toUtf8());
    }
}

// called from SyncEngine
void DiscoveryMainThread::abort() {
    if (_currentDiscoveryDirectoryResult) {
        if (_discoveryJob->_vioMutex.tryLock()) {
            _currentDiscoveryDirectoryResult->code = EIO; // FIXME aborted
            _currentDiscoveryDirectoryResult = 0;
            _discoveryJob->_vioWaitCondition.wakeAll();
            _discoveryJob->_vioMutex.unlock();
        }
    }
    if (_singleDirJob) {
        _singleDirJob->disconnect(SIGNAL(finishedWithError(int,QString)), this);
        _singleDirJob->disconnect(SIGNAL(firstDirectoryPermissions(QString)), this);
        _singleDirJob->disconnect(SIGNAL(finishedWithResult(QLinkedList<csync_vio_file_stat_t*>)), this);
        _singleDirJob->abort();
    }

}

csync_vio_handle_t* DiscoveryJob::remote_vio_opendir_hook (const char *url,
                                    void *userdata)
{
    DiscoveryJob *discoveryJob = static_cast<DiscoveryJob*>(userdata);
    if (discoveryJob) {
        qDebug() << Q_FUNC_INFO << discoveryJob << url << "Calling into main thread...";

        DiscoveryDirectoryResult *directoryResult = new DiscoveryDirectoryResult();
        directoryResult->code = EIO;

        discoveryJob->_vioMutex.lock();
        QString qurl = QString::fromUtf8(url);
        emit discoveryJob->doOpendirSignal(qurl, directoryResult);
        discoveryJob->_vioWaitCondition.wait(&discoveryJob->_vioMutex, ULONG_MAX); // FIXME timeout?
        discoveryJob->_vioMutex.unlock();

        qDebug() << Q_FUNC_INFO << discoveryJob << url << "...Returned from main thread";

        // Upon awakening from the _vioWaitCondition, iterator should be a valid iterator.
        if (directoryResult->code != 0) {
            qDebug() << Q_FUNC_INFO << directoryResult->code << "when opening" << url;
            errno = directoryResult->code;
            return NULL;
        }

        return (csync_vio_handle_t*) directoryResult;
    }
    return NULL;
}


csync_vio_file_stat_t* DiscoveryJob::remote_vio_readdir_hook (csync_vio_handle_t *dhandle,
                                                              void *userdata)
{
    DiscoveryJob *discoveryJob = static_cast<DiscoveryJob*>(userdata);
    if (discoveryJob) {
        DiscoveryDirectoryResult *directoryResult = static_cast<DiscoveryDirectoryResult*>(dhandle);
        if (directoryResult->iterator != directoryResult->list.end()) {
            csync_vio_file_stat_t *file_stat = *(directoryResult->iterator);
            directoryResult->iterator++;
            // Make a copy, csync_update will delete the copy
            return csync_vio_file_stat_copy(file_stat);
        }
    }
    return NULL;
}

void DiscoveryJob::remote_vio_closedir_hook (csync_vio_handle_t *dhandle,  void *userdata)
{
    DiscoveryJob *discoveryJob = static_cast<DiscoveryJob*>(userdata);
    if (discoveryJob) {
        qDebug() << Q_FUNC_INFO << discoveryJob;
        DiscoveryDirectoryResult *directoryResult = static_cast<DiscoveryDirectoryResult*> (dhandle);
        delete directoryResult; // just deletes the struct and the iterator, the data itself is owned by the SyncEngine/DiscoveryMainThread
    }
}

void DiscoveryJob::start() {
    _selectiveSyncBlackList.sort();
    _csync_ctx->checkSelectiveSyncBlackListHook = isInSelectiveSyncBlackListCallBack;
    _csync_ctx->checkSelectiveSyncBlackListData = this;

    _csync_ctx->callbacks.update_callback = update_job_update_callback;
    _csync_ctx->callbacks.update_callback_userdata = this;

    _csync_ctx->callbacks.remote_opendir_hook = remote_vio_opendir_hook;
    _csync_ctx->callbacks.remote_readdir_hook = remote_vio_readdir_hook;
    _csync_ctx->callbacks.remote_closedir_hook = remote_vio_closedir_hook;
    _csync_ctx->callbacks.vio_userdata = this;

    csync_set_log_callback(_log_callback);
    csync_set_log_level(_log_level);
    csync_set_log_userdata(_log_userdata);
    lastUpdateProgressCallbackCall.invalidate();
    int ret = csync_update(_csync_ctx);

    _csync_ctx->checkSelectiveSyncBlackListHook = 0;
    _csync_ctx->checkSelectiveSyncBlackListData = 0;

    _csync_ctx->callbacks.update_callback = 0;
    _csync_ctx->callbacks.update_callback_userdata = 0;

    emit finished(ret);
    deleteLater();
}

}
