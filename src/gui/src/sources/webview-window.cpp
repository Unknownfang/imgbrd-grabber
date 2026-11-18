#include "webview-window.h"
#include <QWebEngineCookieStore>
#include <QWebEnginePage>
#include <QWebEngineProfile>
#include <utility>
#include <ui_webview-window.h>
#include "models/site.h"
#include "network/persistent-cookie-jar.h"


WebViewWindow::WebViewWindow(Site *site, QWidget *parent)
	: QDialog(parent), ui(new Ui::WebViewWindow), m_site(site)
{
	setAttribute(Qt::WA_DeleteOnClose);
	ui->setupUi(this);

	const QUrl url(m_site->fixUrl("/"));

	// Build web view
	auto *profile = new QWebEngineProfile(ui->webView);
	profile->setPersistentCookiesPolicy(QWebEngineProfile::NoPersistentCookies);
	profile->setHttpUserAgent(m_site->userAgent());
	auto *page = new QWebEnginePage(profile, ui->webView);
	page->load(url);
	ui->webView->setPage(page);

	// Set the web view's cookies based on the site's cookie jar
	QWebEngineCookieStore *cookieStore = profile->cookieStore();
	auto *cookieJar = m_site->cookieJar();
	for (const auto &cookie : cookieJar->getAllCookies()) {
		cookieStore->setCookie(cookie);
	}

	// Timer to de-bounce the saving of the cookies on disk
	m_saveCookies.setSingleShot(true);
	m_saveCookies.setInterval(1000);
	connect(&m_saveCookies, &QTimer::timeout, [=]() {
		cookieJar->save();
	});

	// Cookie listener to automatically sync the web view cookies with the site's cookie jar
	connect(cookieStore, &QWebEngineCookieStore::cookieRemoved, [=](const QNetworkCookie &cookie) {
		for (int i = m_webViewCookies.count() - 1; i >= 0; --i) {
			if (m_webViewCookies[i].hasSameIdentifier(cookie)) {
				m_webViewCookies.removeAt(i);
			}
		}

		if (m_cookies.isEmpty()) {
			cookieJar->deleteCookie(cookie);
			m_saveCookies.start();
		}
	});
	connect(cookieStore, &QWebEngineCookieStore::cookieAdded, [=](const QNetworkCookie &cookie) {
		bool replaced = false;
		for (auto &existing : m_webViewCookies) {
			if (existing.hasSameIdentifier(cookie)) {
				existing = cookie;
				replaced = true;
			}
		}
		if (!replaced) {
			m_webViewCookies.append(cookie);
		}

		if (m_cookies.isEmpty() || m_cookies.contains(cookie.name())) {
			cookieJar->insertCookie(cookie);
			m_saveCookies.start();

			if (!m_cookies.isEmpty()) {
				emit cookieFound(cookie.name());
				close();
			}
		}
	});

	// Handle the signals from the basic UI
	connect(ui->lineUrl, &QLineEdit::returnPressed, [this, page]() {
		page->load(QUrl(ui->lineUrl->text()));
	});
	connect(ui->buttonGo, &QPushButton::clicked, [this, page]() {
		page->load(QUrl(ui->lineUrl->text()));
	});
	connect(ui->buttonCookieMonster, &QPushButton::clicked, [this]() {
		QList<QNetworkCookie> siteCookies;
		for (const QNetworkCookie &cookie : m_webViewCookies) {
			if (cookieMatchesSite(cookie)) {
				siteCookies.append(cookie);
			}
		}
		emit cookieMonsterClicked(siteCookies);
	});
	connect(page, &QWebEnginePage::urlChanged, [this](const QUrl &url) {
		ui->lineUrl->setText(url.toString());
	});
}

WebViewWindow::WebViewWindow(Site *site, QStringList cookies, QWidget *parent)
	: WebViewWindow(site, parent)
{
	m_cookies = std::move(cookies);
}

bool WebViewWindow::cookieMatchesSite(const QNetworkCookie &cookie) const
{
	const QString host = m_site->fixUrl("/").host().toLower();
	QString domain = cookie.domain().toLower();
	if (domain.startsWith('.')) {
		domain = domain.mid(1);
	}

	// Cookies without a domain are host-only cookies, keep them
	if (domain.isEmpty() || host.isEmpty()) {
		return true;
	}

	// Same host, cookie set on a parent domain, or cookie set on a subdomain of the site
	return host == domain
		|| host.endsWith("." + domain)
		|| domain.endsWith("." + host);
}

WebViewWindow::~WebViewWindow()
{
	delete ui;

	// If we are still pending a "save cookies" operation, do it now
	if (m_saveCookies.isActive()) {
		m_saveCookies.stop();
		m_site->cookieJar()->save();
	}
}
