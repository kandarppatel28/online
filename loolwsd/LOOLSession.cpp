/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/*
 * This file is part of the LibreOffice project.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include <cassert>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>

#define LOK_USE_UNSTABLE_API
#include <LibreOfficeKit/LibreOfficeKit.h>
#include <LibreOfficeKit/LibreOfficeKitEnums.h>

#include <Poco/Buffer.h>
#include <Poco/Process.h>
#include <Poco/Random.h>
#include <Poco/String.h>
#include <Poco/StringTokenizer.h>
#include <Poco/URI.h>
#include <Poco/Util/Application.h>

#include "LOKitHelper.hpp"
#include "LOOLProtocol.hpp"
#include "LOOLSession.hpp"
#include "LOOLWSD.hpp"
#include "TileCache.hpp"
#include "Util.hpp"

using namespace LOOLProtocol;

using Poco::Buffer;
using Poco::Net::WebSocket;
using Poco::Process;
using Poco::ProcessHandle;
using Poco::Random;
using Poco::StringTokenizer;
using Poco::Thread;
using Poco::UInt64;
using Poco::URI;
using Poco::Util::Application;

std::map<UInt64, LOOLSession*> LOOLSession::_childToClient;

LOOLSession::LOOLSession(WebSocket& ws, LibreOfficeKit *loKit) :
    _docURL(""),
    _ws(ws),
    _toChildProcess(false),
    _peerWs(nullptr),
    _childId(0),
    _loKit(loKit),
    _loKitDocument(NULL)
{
    std::cout << Util::logPrefix() << "LOOLSesstion ctor this=" << this << " ws=" << &_ws << std::endl;
}

LOOLSession::~LOOLSession()
{
    std::cout << Util::logPrefix() << "LOOLSesstion dtor this=" << this << std::endl;
    _ws.shutdown();
    if (_loKitDocument)
        _loKitDocument->pClass->destroy(_loKitDocument);
}

bool LOOLSession::handleInput(char *buffer, int length)
{
    Application& app = Application::instance();

    if (haveSeparateProcess())
    {
        forwardRequest(buffer, length);
        return true;
    }

    char *endl = (char *) memchr(buffer, '\n', length);
    std::string commandline;
    if (endl == nullptr)
        commandline = std::string(buffer, length);
    else
        commandline = std::string(buffer, endl-buffer);

    app.logger().information(Util::logPrefix() + "Input: '" + commandline + "'" + (endl == nullptr ? "" : " ..."));

    StringTokenizer tokens(commandline, " ", StringTokenizer::TOK_IGNORE_EMPTY | StringTokenizer::TOK_TRIM);

    if (toChildProcess())
    {
        assert(_peerWs != nullptr);
        _peerWs->sendFrame(buffer, length, WebSocket::FRAME_BINARY);
    }
    else if (tokens[0] == "child")
    {
        if (_peerWs != nullptr ||
            isChildProcess())
        {
            sendTextFrame("error: cmd=child kind=invalid");
            return false;
        }
        if (tokens.count() != 2)
        {
            sendTextFrame("error: cmd=child kind=syntax");
            return false;
        }
        UInt64 childId = std::stoull(tokens[1]);
        app.logger().information(Util::logPrefix() + "childId=" + std::to_string(childId));
        if (_childToClient.find(childId) == _childToClient.end())
        {
            sendTextFrame("error: cmd=child kind=notfound");
            return false;
        }
        assert(_childToClient[childId]->haveSeparateProcess());
        assert(_childToClient[childId]->_peerWs == nullptr);
        _childToClient[childId]->_peerWs = &_ws;
        _peerWs = &_childToClient[childId]->_ws;
        for (auto i : _childToClient[childId]->_backLog)
        {
            std::cout << Util::logPrefix() << "Sending backlog: '" << std::string(i->begin(), i->size()) << "'" << std::endl;
            _ws.sendFrame(i->begin(), i->size(), WebSocket::FRAME_BINARY);
            delete i;
        }
        _toChildProcess = true;
    }
    else if (tokens[0] == "load")
    {
        if (_docURL != "")
        {
            sendTextFrame("error: cms=load kind=docalreadyloaded");
            return false;
        }
        return loadDocument(buffer, length, tokens);
    }
    else if (_docURL == "")
    {
        sendTextFrame("error: cmd=" + tokens[0] + " kind=nodocloaded");
        return false;
    }
    else if (tokens[0] == "status")
    {
        return getStatus(buffer, length);
    }
    else if (tokens[0] == "tile")
    {
        sendTile(buffer, length, tokens);
    }
    else
    {
        // All other commands are such that they always require a
        // LibreOfficeKitDocument session, i.e. need to be handled in
        // a child process.

        if (!isChildProcess())
        {
            if (tokens[0] != "key" &&
                tokens[0] != "mouse" &&
                tokens[0] != "uno" &&
                tokens[0] != "selecttext" &&
                tokens[0] != "selectgraphic" &&
                tokens[0] != "resetselection" &&
                tokens[0] != "saveas")
            {
                sendTextFrame("error: cmd=" + tokens[0] + " kind=unknown");
                return false;
            }

            forkOff();
            forwardRequest(buffer, length);
            return true;
        }

        if (tokens[0] == "key")
        {
            return keyEvent(buffer, length, tokens);
        }
        else if (tokens[0] == "mouse")
        {
            return mouseEvent(buffer, length, tokens);
        }
        else if (tokens[0] == "uno")
        {
            return unoCommand(buffer, length, tokens);
        }
        else if (tokens[0] == "selecttext")
        {
            return selectText(buffer, length, tokens);
        }
        else if (tokens[0] == "selectgraphic")
        {
            return selectGraphic(buffer, length, tokens);
        }
        else if (tokens[0] == "resetselection")
        {
            return resetSelection(buffer, length, tokens);
        }
        else if (tokens[0] == "saveas")
        {
            return saveAs(buffer, length, tokens);
        }
        else
            assert(false);
    }
    return true;
}

bool LOOLSession::haveSeparateProcess() const
{
    return _childId != 0;
}

bool LOOLSession::toChildProcess() const
{
    return _toChildProcess;
}

void LOOLSession::sendTextFrame(const std::string& text)
{
    _ws.sendFrame(text.data(), text.size());
}

void LOOLSession::sendBinaryFrame(const char *buffer, int length)
{
    _ws.sendFrame(buffer, length, WebSocket::FRAME_BINARY);
}

extern "C"
{
    static void myCallback(int nType, const char* pPayload, void* pData)
    {
        LOOLSession *srv = reinterpret_cast<LOOLSession *>(pData);

        switch ((LibreOfficeKitCallbackType) nType)
        {
        case LOK_CALLBACK_INVALIDATE_TILES:
            srv->sendTextFrame("invalidatetiles: " + std::string(pPayload));
            break;
        case LOK_CALLBACK_INVALIDATE_VISIBLE_CURSOR:
            srv->sendTextFrame("invalidatecursor:");
            break;
        case LOK_CALLBACK_TEXT_SELECTION:
            srv->sendTextFrame("textselection: " + std::string(pPayload));
            break;
        case LOK_CALLBACK_TEXT_SELECTION_START:
            srv->sendTextFrame("textselectionstart: " + std::string(pPayload));
            break;
        case LOK_CALLBACK_TEXT_SELECTION_END:
            srv->sendTextFrame("textselectionend: " + std::string(pPayload));
            break;
        case LOK_CALLBACK_CURSOR_VISIBLE:
            srv->sendTextFrame("cursorvisible: " + std::string(pPayload));
            break;
        case LOK_CALLBACK_GRAPHIC_SELECTION:
            srv->sendTextFrame("graphicselection: " + std::string(pPayload));
            break;
        case LOK_CALLBACK_HYPERLINK_CLICKED:
            srv->sendTextFrame("hyperlinkclicked: " + std::string(pPayload));
            break;
        }
    }
}

bool LOOLSession::loadDocument(const char *buffer, int length, StringTokenizer& tokens)
{
    if (tokens.count() != 2)
    {
        sendTextFrame("error: cmd=load kind=syntax");
        return false;
    }

    if (tokens[1].find("url=") == 0)
        _docURL = tokens[1].substr(strlen("url="));
    else
        _docURL = tokens[1];

    _tileCache.reset(new TileCache(_docURL));

    if (isChildProcess())
    {
        if ((_loKitDocument = _loKit->pClass->documentLoad(_loKit, _docURL.c_str())) == NULL)
        {
            sendTextFrame("error: cmd=load kind=failed");
            return false;
        }

        if (!getStatus(buffer, length))
            return false;
        _loKitDocument->pClass->registerCallback(_loKitDocument, myCallback, this);
    }        

    return true;
}

bool LOOLSession::getStatus(const char *buffer, int length)
{
    std::string status = _tileCache->getStatus();
    if (status.size() > 0)
    {
        sendTextFrame(status);
        return true;
    }

    if (!isChildProcess())
    {
        forkOff();
        forwardRequest(buffer, length);
        return true;
    }

    status = "status: " + LOKitHelper::documentStatus(_loKitDocument);
    _tileCache->saveStatus(status);

    sendTextFrame(status);

    return true;
}

void LOOLSession::sendTile(const char *buffer, int length, StringTokenizer& tokens)
{
    int width, height, tilePosX, tilePosY, tileWidth, tileHeight;

    if (tokens.count() != 7 ||
        !getTokenInteger(tokens[1], "width", width) ||
        !getTokenInteger(tokens[2], "height", height) ||
        !getTokenInteger(tokens[3], "tileposx", tilePosX) ||
        !getTokenInteger(tokens[4], "tileposy", tilePosY) ||
        !getTokenInteger(tokens[5], "tilewidth", tileWidth) ||
        !getTokenInteger(tokens[6], "tileheight", tileHeight))
    {
        sendTextFrame("error: cmd=tile kind=syntax");
        return;
    }

    if (width <= 0 ||
        height <= 0 ||
        tilePosX < 0 ||
        tilePosY < 0 ||
        tileWidth <= 0 ||
        tileHeight <= 0)
    {
        sendTextFrame("error: cmd=tile kind=invalid");
        return;
    }

    std::string response = "tile: " + Poco::cat(std::string(" "), tokens.begin() + 1, tokens.end()) + "\n";

    std::vector<char> output;
    output.reserve(4 * width * height);
    output.resize(response.size());
    memcpy(output.data(), response.data(), response.size());

    std::unique_ptr<std::fstream> cachedTile = _tileCache->lookupTile(width, height, tilePosX, tilePosY, tileWidth, tileHeight);
    if (cachedTile && cachedTile->is_open())
    {
        cachedTile->seekg(0, std::ios_base::end);
        size_t pos = output.size();
        std::streamsize size = cachedTile->tellg();
        output.resize(pos + size);
        cachedTile->seekg(0, std::ios_base::beg);
        cachedTile->read(output.data() + pos, size);
        cachedTile->close();

        sendBinaryFrame(output.data(), output.size());

        return;
    }

    if (!isChildProcess())
    {
        forkOff();
        forwardRequest(buffer, length);
        return;
    }

    unsigned char *pixmap = new unsigned char[4 * width * height];
    _loKitDocument->pClass->paintTile(_loKitDocument, pixmap, width, height, tilePosX, tilePosY, tileWidth, tileHeight);

    if (!Util::encodePNGAndAppendToBuffer(pixmap, width, height, output))
    {
        sendTextFrame("error: cmd=tile kind=failure");
        return;
    }

    delete[] pixmap;

    _tileCache->saveTile(width, height, tilePosX, tilePosY, tileWidth, tileHeight, output.data() + response.size(), output.size() - response.size());

    sendBinaryFrame(output.data(), output.size());
}

bool LOOLSession::keyEvent(const char *buffer, int length, Poco::StringTokenizer& tokens)
{
    assert(isChildProcess());

    int type, charcode, keycode;

    if (tokens.count() != 4 ||
        !getTokenInteger(tokens[1], "type", type) ||
        !getTokenInteger(tokens[2], "char", charcode) ||
        !getTokenInteger(tokens[3], "key", keycode))
    {
        sendTextFrame("error: cmd=key kind=syntax");
        return false;
    }

    _loKitDocument->pClass->postKeyEvent(_loKitDocument, type, charcode, keycode);

    return true;
}

bool LOOLSession::mouseEvent(const char *buffer, int length, Poco::StringTokenizer& tokens)
{
    assert(isChildProcess());

    int type, x, y, count;

    if (tokens.count() != 5 ||
        !getTokenInteger(tokens[1], "type", type) ||
        !getTokenInteger(tokens[2], "x", x) ||
        !getTokenInteger(tokens[3], "y", y) ||
        !getTokenInteger(tokens[4], "count", count))
    {
        sendTextFrame("error: cmd=mouse kind=syntax");
        return false;
    }

    _loKitDocument->pClass->postMouseEvent(_loKitDocument, type, x, y, count);

    return true;
}

bool LOOLSession::unoCommand(const char *buffer, int length, Poco::StringTokenizer& tokens)
{
    assert(isChildProcess());

    if (tokens.count() == 1)
    {
        sendTextFrame("error: cmd=uno kind=syntax");
        return false;
    }

    _loKitDocument->pClass->postUnoCommand(_loKitDocument, buffer + strlen("uno "));

    return true;
}

bool LOOLSession::selectText(const char *buffer, int length, Poco::StringTokenizer& tokens)
{
    assert(isChildProcess());

    int type, x, y;

    if (tokens.count() != 4 ||
        !getTokenInteger(tokens[1], "type", type) ||
        !getTokenInteger(tokens[2], "x", x) ||
        !getTokenInteger(tokens[3], "y", y))
    {
        sendTextFrame("error: cmd=selecttext kind=syntax");
        return false;
    }

    _loKitDocument->pClass->setTextSelection(_loKitDocument, type, x, y);

    return true;
}

bool LOOLSession::selectGraphic(const char *buffer, int length, Poco::StringTokenizer& tokens)
{
    assert(isChildProcess());

    int type, x, y;

    if (tokens.count() != 4 ||
        !getTokenInteger(tokens[1], "type", type) ||
        !getTokenInteger(tokens[2], "x", x) ||
        !getTokenInteger(tokens[3], "y", y))
    {
        sendTextFrame("error: cmd=selectghraphic kind=syntax");
        return false;
    }

    _loKitDocument->pClass->setGraphicSelection(_loKitDocument, type, x, y);

    return true;
}

bool LOOLSession::resetSelection(const char *buffer, int length, Poco::StringTokenizer& tokens)
{
    assert(isChildProcess());

    if (tokens.count() != 1)
    {
        sendTextFrame("error: cmd=resetselection kind=syntax");
        return false;
    }

    _loKitDocument->pClass->resetSelection(_loKitDocument);

    return true;
}

bool LOOLSession::saveAs(const char *buffer, int length, Poco::StringTokenizer& tokens)
{
    assert(isChildProcess());

    std::string url, format, filterOptions;

    if (tokens.count() < 4 ||
        !getTokenString(tokens[1], "url", url) ||
        !getTokenString(tokens[2], "format", format) ||
        !getTokenString(tokens[3], "options", filterOptions))
    {
        sendTextFrame("error: cmd=saveas kind=syntax");
        return false;
    }

    URI::decode(url, url, true);
    URI::decode(format, format, true);

    if (tokens.count() > 4)
        filterOptions += Poco::cat(std::string(" "), tokens.begin() + 4, tokens.end());

    _loKitDocument->pClass->saveAs(_loKitDocument, url.c_str(), format.c_str(), filterOptions.c_str());

    return true;
}

void LOOLSession::forkOff()
{
    assert(!isChildProcess());

    Process::Args args;
    args.push_back("--port=" + std::to_string(LOOLWSD::portNumber));
    args.push_back("--lopath=" + LOOLWSD::loPath);

    Random rng;
    
    _childId = (((UInt64)rng.next()) << 32) | rng.next() | 1;

    _childToClient[_childId] = this;

    args.push_back("--child=" + std::to_string(_childId));

    Application::instance().logger().information(Util::logPrefix() + "Launching child: " + Poco::cat(std::string(" "), args.begin(), args.end()));

    ProcessHandle child = Process::launch(Application::instance().commandPath(), args);

    std::string loadRequest = "load " + _docURL;
    forwardRequest(loadRequest.c_str(), loadRequest.size());
}

void LOOLSession::forwardRequest(const char *buffer, int length)
{
    if (_peerWs == nullptr)
    {
        _backLog.push_back(new Poco::Buffer<char>(buffer, length));
        return;
    }
    _peerWs->sendFrame(buffer, length, WebSocket::FRAME_BINARY);
}

bool LOOLSession::isChildProcess() const
{
    return _loKit != nullptr;
}


/* vim:set shiftwidth=4 softtabstop=4 expandtab: */
