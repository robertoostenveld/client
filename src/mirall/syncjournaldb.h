/*
 * Copyright (C) by Klaas Freitag <freitag@owncloud.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 2 of the License.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
 * for more details.
 */

#ifndef SYNCJOURNALDB_H
#define SYNCJOURNALDB_H

#include <QObject>
#include <qmutex.h>
#include <QSqlDatabase>

namespace Mirall {
class SyncJournalFileRecord;

class SyncJournalDb : public QObject
{
    Q_OBJECT
public:
    explicit SyncJournalDb(const QString& path, QObject *parent = 0);
    SyncJournalFileRecord getFileRecord( const QString& filename );
    bool setFileRecord( const SyncJournalFileRecord& record );
    bool deleteFileRecord( const QString& filename );
    int getFileRecordCount();
    bool exists();

signals:

public slots:

private:
    qint64 getPHash(const QString& ) const;
    bool checkConnect();
    QSqlDatabase _db;
    QString _dbFile;
    QMutex _mutex; // Public functions are protected with the mutex.

};

}  // namespace Mirall
#endif // SYNCJOURNALDB_H