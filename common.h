#ifndef H_COMMON
#define H_COMMON

#include <QtCore>
#include <QHostAddress>

/*
 * Handle program settings, and defaults values
 */
class Settings : private QSettings {
	public:
		Settings ();

		/* network settings */
		QString name (void) const;
		void setName (QString & name);

		quint16 tcpPort (void) const;
		void setTcpPort (quint16 port);

		/* download path/confirmation settings */
		QString downloadPath (void) const;
		void setDownloadPath (const QString & path);

		bool alwaysDownload (void) const;
		void setAlwaysDownload (bool always);
};


/*
 * Error messages
 */
class Message {
	public:
		static void error (const QString & title, const QString & message);
		static void warning (const QString & title, const QString & message);
};

#endif

