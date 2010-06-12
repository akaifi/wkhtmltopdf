//-*- mode: c++; tab-width: 4; indent-tabs-mode: t; c-file-style: "stroustrup"; -*-
// This file is part of wkhtmltopdf.
//
// wkhtmltopdf is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// wkhtmltopdf is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with wkhtmltopdf.  If not, see <http://www.gnu.org/licenses/>.
#include "commandlineparser.hh"
#include "pageconverter.hh"
#include "progressfeedback.hh"
#include "settings.hh"
#include <QCleanlooksStyle>
#include <QCommonStyle>
#include <QPainter>
#include <QStyleOption>
#include <QWebFrame>
#include <QtPlugin>
#include <cstdlib>
#include <iostream>
#include <qapplication.h>
#include <qglobal.h>
#include <string.h>

using namespace wkhtmltopdf::settings;
using namespace wkhtmltopdf;

#ifdef QT_STATIC
//When doing a static build, we need to load the plugins to make images work
Q_IMPORT_PLUGIN(qjpeg)
Q_IMPORT_PLUGIN(qgif)
Q_IMPORT_PLUGIN(qtiff)
Q_IMPORT_PLUGIN(qmng)
#endif

/*!
 * State mashine driven, shell like parser. This is used for
 * reading commandline options from stdin
 * \param buff the line to parse
 * \param nargc on return will hold the number of arguments read
 * \param nargv on return will hold the argumenst read and be NULL terminated
 */
enum State {skip, tok, q1, q2, q1_esc, q2_esc, tok_esc};
void parseString(char * buff, int &nargc, char **nargv) {
	State state = skip;
	int write_start=0;
	int write=0;
	for (int read=0; buff[read]!='\0'; ++read) {
		State next_state=state;
		switch (state) {
		case skip:
			//Whitespace skipping state
			if (buff[read]!=' ' && buff[read]!='\t' && buff[read]!='\r' && buff[read]!='\n') {
				--read;
				next_state=tok;
			}
			break;
		case tok:
			//Normal toking reading state
			if (buff[read]=='\'') next_state=q1;
			else if (buff[read]=='"') next_state=q2;
			else if (buff[read]=='\\') next_state=tok_esc;
			else if (buff[read]==' ' || buff[read]=='\t' || buff[read]=='\n' || buff[read]=='\r') {
				next_state=skip;
				if (write_start != write) {
					buff[write++]='\0';
					nargv[nargc++] = buff+write_start;
					if (nargc > 998) exit(1);
				}
				write_start = write;
			} else buff[write++] = buff[read];
			break;
		case q1:
			//State parsing a single qoute argument
			if (buff[read]=='\'') next_state=tok;
			else if (buff[read]=='\\') next_state=q1_esc;
			else buff[write++] = buff[read];
			break;
		case q2:
			//State parsing a double qoute argument
			if (buff[read]=='"') next_state=tok;
			else if (buff[read]=='\\') next_state=q2_esc;
			else buff[write++] = buff[read];
			break;
		case tok_esc:
			//Escape one char and return to the tokan parsing state
			next_state=tok;
			buff[write++] = buff[read];
			break;
		case q1_esc:
			//Espace one char and return to the single quote parsing state
			next_state=q1;
			buff[write++] = buff[read];
			break;
		case q2_esc:
			//Escape one char and return to the double qoute parsing state
			next_state=q2;
			buff[write++] = buff[read];
			break;
		}
		state=next_state;
	}
	//Remember the last parameter
	if (write_start != write) {
		buff[write++]='\0';
		nargv[nargc++] = buff+write_start;
	}
	nargv[nargc]=NULL;
}

class MyLooksStyle: public QCleanlooksStyle {
public:
	//Settings & settings;
	
	typedef QCleanlooksStyle parent_t;
	
	//MyLooksStyle(Settings & s): settings(s) {}
	
	void drawPrimitive( PrimitiveElement element, const QStyleOption * option, QPainter * painter, const QWidget * widget = 0 ) const {
		painter->setBrush(Qt::white);
		painter->setPen(QPen(Qt::black, 0.7));
		QRect r = option->rect;
		if (element == QStyle::PE_PanelLineEdit) {
			painter->drawRect(r);
		} else if(element == QStyle::PE_IndicatorCheckBox) {
			painter->drawRect(r);
			if (true) { //(!settings.produceForms && (option->state & QStyle::State_On)) {
				r.translate(r.width()*0.075, r.width()*0.075);
				painter->drawLine(r.topLeft(), r.bottomRight());
				painter->drawLine(r.topRight(), r.bottomLeft());
			}
		} else if(element == QStyle::PE_IndicatorRadioButton) {
			painter->drawEllipse(r);
			if (true) { //!settings.produceForms && (option->state & QStyle::State_On)) {
				r.translate(r.width()*0.20, r.width()*0.20);
				r.setWidth(r.width()*0.70);
				r.setHeight(r.height()*0.70);
				painter->setBrush(Qt::black);
				painter->drawEllipse(r);
			}
		} else {
			parent_t::drawPrimitive(element, option, painter, widget);
		}
	}
};

int handleError(bool success, int errorCode) {
	QHash<int, const char *> cm;
	cm[400] = "Bad Request";
	cm[401] = "Unauthorized";
	cm[402] = "Payment Required";
	cm[403] = "Forbidden";
	cm[404] = "Page not found";
	cm[405] = "Method Not Allowed";
	cm[500] = "Internal Server Error";
	cm[501] = "Not Implemented";
	cm[503] = "Service Unavailable";
	cm[505] = "HTTP Version Not Supported";
	QHash<int, int> ce;
	ce[404] = 2;
	ce[401] = 3;
	if (errorCode) {
		int c = EXIT_FAILURE;
		if (ce.contains(errorCode)) c = ce[errorCode];
		const char * m = "";
		if (cm.contains(errorCode)) m = cm[errorCode];
		fprintf(stderr, "Exit with code %d do to http error: %d %s\n", c, errorCode, m);
		return c;
	}
	return success?EXIT_SUCCESS:EXIT_FAILURE;
}

int main(int argc, char * argv[]) {
	//This will store all our settings
	Global globalSettings;
	QList<Page> pageSettings;
	//Create a command line parser to parse commandline arguments
	CommandLineParser parser(globalSettings, pageSettings);

	//Setup default values in settings
	//parser.loadDefaults();

	//Parse the arguments
	parser.parseArguments(argc, (const char**)argv);

	//Construct QApplication required for printing
	bool use_graphics=true;
#if defined(Q_WS_X11) || defined(Q_WS_MACX)
#ifdef __EXTENSIVE_WKHTMLTOPDF_QT_HACK__
	use_graphics=globalSettings.useGraphics;
	if (!use_graphics) QApplication::setGraphicsSystem("raster");
#endif
#endif
	QApplication a(argc, argv, use_graphics);
	a.setStyle(new MyLooksStyle());

	if (parser.readArgsFromStdin) {
		char buff[20400];
		char *nargv[1000];
		nargv[0] = argv[0];
		for (int i=0; i < argc; ++i) nargv[i] = argv[i];
		while (fgets(buff,20398,stdin)) {
			int nargc=argc;
			parseString(buff,nargc,nargv);

			Global globalSettings;
			QList<Page> pageSettings;
			//Create a command line parser to parse commandline arguments
			CommandLineParser parser(globalSettings, pageSettings);
			//Setup default values in settings
			//parser.loadDefaults();
			//Parse the arguments
			parser.parseArguments(nargc, (const char**)nargv, true);
			
			PageConverter converter(globalSettings);
			ProgressFeedback feedback(converter);
			foreach(const Page & page, pageSettings) 
				converter.addResource(page);

			if (!converter.convert())
				exit(EXIT_FAILURE);
		}
		exit(EXIT_SUCCESS);
	}
	//Create the actual page converter to convert the pages
	PageConverter converter(globalSettings);
	ProgressFeedback feedback(converter);
	foreach(const Page & page, pageSettings) 
		converter.addResource(page);

	bool success = converter.convert();
	return handleError(success, converter.httpErrorCode());
}