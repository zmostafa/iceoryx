// Copyright (c) 2019 by Robert Bosch GmbH. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "iceoryx_utils/internal/posix_wrapper/message_queue.hpp"
#include "iceoryx_utils/cxx/smart_c.hpp"

#include <chrono>
#include <cstdlib>

namespace iox
{
namespace posix
{
MessageQueue::MessageQueue()
{
    this->m_isInitialized = false;
    this->m_errorValue = IpcChannelError::NOT_INITIALIZED;
}

MessageQueue::MessageQueue(const std::string& name, const IpcChannelMode mode, const IpcChannelSide channelSide)
    : m_name{name}
    , m_channelSide(channelSide)
{
    // fields have a different order in QNX,
    // so we need to initialize by name
    m_attributes.mq_flags = (mode == IpcChannelMode::NON_BLOCKING) ? O_NONBLOCK : 0;
    m_attributes.mq_maxmsg = MAX_MSG_NUMBER;
    m_attributes.mq_msgsize = MAX_MESSAGE_SIZE;
    m_attributes.mq_curmsgs = 0;
#ifdef __QNX__
    m_attributes.mq_recvwait = 0;
    m_attributes.mq_sendwait = 0;
#endif
    auto openResult = open(name, mode, channelSide);

    if (!openResult.has_error())
    {
        this->m_isInitialized = true;
        this->m_errorValue = IpcChannelError::UNDEFINED;
        this->m_mqDescriptor = openResult.get_value();
    }
    else
    {
        this->m_isInitialized = false;
        this->m_errorValue = openResult.get_error();
    }
}

MessageQueue::MessageQueue(MessageQueue&& other)
{
    *this = std::move(other);
}

MessageQueue::~MessageQueue()
{
    if (destroy().has_error())
    {
        std::cerr << "unable to cleanup message queue \"" << m_name << "\" in the destructor" << std::endl;
    }
}

MessageQueue& MessageQueue::operator=(MessageQueue&& other)
{
    if (this != &other)
    {
        if (destroy().has_error())
        {
            std::cerr << "unable to cleanup message queue \"" << m_name
                      << "\" during move operation - resource leaks are possible!" << std::endl;
        }

        m_name = std::move(other.m_name);
        m_attributes = std::move(other.m_attributes);
        m_mqDescriptor = std::move(other.m_mqDescriptor);
        m_channelSide = std::move(other.m_channelSide);
        other.m_mqDescriptor = INVALID_DESCRIPTOR;
    }

    return *this;
}

cxx::expected<IpcChannelError> MessageQueue::destroy()
{
    if (m_mqDescriptor != INVALID_DESCRIPTOR)
    {
        auto closeCall = close();
        if (closeCall.has_error())
        {
            m_mqDescriptor = INVALID_DESCRIPTOR;
            return closeCall;
        }
        auto unlinkCall = unlink();
        if (unlinkCall.has_error())
        {
            m_mqDescriptor = INVALID_DESCRIPTOR;
            return unlinkCall;
        }
    }

    m_mqDescriptor = INVALID_DESCRIPTOR;
    return cxx::success<void>();
}

cxx::expected<IpcChannelError> MessageQueue::send(const std::string& msg)
{
    if (msg.size() > MAX_MESSAGE_SIZE)
    {
        return cxx::error<IpcChannelError>(IpcChannelError::MESSAGE_TOO_LONG);
    }

    auto mqCall = cxx::makeSmartC(mq_send,
                                  cxx::ReturnMode::PRE_DEFINED_ERROR_CODE,
                                  {ERROR_CODE},
                                  {},
                                  m_mqDescriptor,
                                  msg.c_str(),
                                  msg.size() + 1, // +1 for the \0 at the end
                                  1);

    if (mqCall.hasErrors())
    {
        return createErrorFromErrnum(mqCall.getErrNum());
    }

    return cxx::success<void>();
}

cxx::expected<std::string, IpcChannelError> MessageQueue::receive()
{
    char message[MAX_MESSAGE_SIZE];
    auto mqCall = cxx::makeSmartC(mq_receive,
                                  cxx::ReturnMode::PRE_DEFINED_ERROR_CODE,
                                  {static_cast<ssize_t>(ERROR_CODE)},
                                  {},
                                  m_mqDescriptor,
                                  &(message[0]),
                                  MAX_MESSAGE_SIZE,
                                  nullptr);

    if (mqCall.hasErrors())
    {
        return createErrorFromErrnum(mqCall.getErrNum());
    }

    return cxx::success<std::string>(std::string(&(message[0])));
}

cxx::expected<int32_t, IpcChannelError>
MessageQueue::open(const std::string& name, const IpcChannelMode mode, const IpcChannelSide channelSide)
{
    if (name.empty() || name.at(0) != '/')
    {
        return cxx::error<IpcChannelError>(IpcChannelError::INVALID_CHANNEL_NAME);
    }

    int32_t openFlags = O_RDWR;
    openFlags |= (mode == IpcChannelMode::NON_BLOCKING) ? O_NONBLOCK : 0;
    if (channelSide == IpcChannelSide::SERVER)
    {
        openFlags |= O_CREAT;
    }

    // the mask will be applied to the permissions, therefore we need to set it to 0
    mode_t umaskSaved = umask(0);

    auto mqCall = cxx::makeSmartC(mq_open,
                                  cxx::ReturnMode::PRE_DEFINED_ERROR_CODE,
                                  {ERROR_CODE},
                                  {},
                                  name.c_str(),
                                  openFlags,
                                  m_filemode,
                                  &m_attributes);

    umask(umaskSaved);

    if (!mqCall.hasErrors())
    {
        return cxx::success<int32_t>(mqCall.getReturnValue());
    }
    else
    {
        return createErrorFromErrnum(mqCall.getErrNum());
    }
}

cxx::expected<IpcChannelError> MessageQueue::close()
{
    auto mqCall = cxx::makeSmartC(mq_close, cxx::ReturnMode::PRE_DEFINED_ERROR_CODE, {ERROR_CODE}, {}, m_mqDescriptor);

    if (mqCall.hasErrors())
    {
        return createErrorFromErrnum(mqCall.getErrNum());
    }

    return cxx::success<void>();
}

cxx::expected<IpcChannelError> MessageQueue::unlink()
{
    if (m_channelSide == IpcChannelSide::CLIENT)
    {
        return cxx::success<void>();
    }
    else
    {
        auto mqCall =
            cxx::makeSmartC(mq_unlink, cxx::ReturnMode::PRE_DEFINED_ERROR_CODE, {ERROR_CODE}, {}, m_name.c_str());
        if (mqCall.hasErrors())
        {
            return createErrorFromErrnum(mqCall.getErrNum());
        }

        return cxx::success<void>();
    }
}

cxx::expected<std::string, IpcChannelError> MessageQueue::timedReceive(const units::Duration& timeout)
{
    timespec timeOut = timeout.timespec(units::TimeSpecReference::Epoch);
    char message[MAX_MESSAGE_SIZE];

    auto mqCall = cxx::makeSmartC(mq_timedreceive,
                                  cxx::ReturnMode::PRE_DEFINED_ERROR_CODE,
                                  {static_cast<ssize_t>(ERROR_CODE)},
                                  {TIMEOUT_ERRNO},
                                  m_mqDescriptor,
                                  &(message[0]),
                                  MAX_MESSAGE_SIZE,
                                  nullptr,
                                  &timeOut);

    if (mqCall.hasErrors())
    {
        return createErrorFromErrnum(mqCall.getErrNum());
    }
    else if (mqCall.getErrNum() == TIMEOUT_ERRNO)
    {
        return createErrorFromErrnum(ETIMEDOUT);
    }

    return cxx::success<std::string>(std::string(&(message[0])));
}

cxx::expected<IpcChannelError> MessageQueue::timedSend(const std::string& msg, const units::Duration& timeout)
{
    if (msg.size() > MAX_MESSAGE_SIZE)
    {
        std::cerr << "the message \"" << msg << "\" which should be sent to the message queue \"" << m_name
                  << "\" is too long" << std::endl;
        return cxx::error<IpcChannelError>(IpcChannelError::MESSAGE_TOO_LONG);
    }

    timespec timeOut = timeout.timespec(units::TimeSpecReference::Epoch);

    auto mqCall = cxx::makeSmartC(mq_timedsend,
                                  cxx::ReturnMode::PRE_DEFINED_ERROR_CODE,
                                  {ERROR_CODE},
                                  {TIMEOUT_ERRNO},
                                  m_mqDescriptor,
                                  msg.c_str(),
                                  msg.size(),
                                  1,
                                  &timeOut);

    if (mqCall.hasErrors())
    {
        return createErrorFromErrnum(mqCall.getErrNum());
    }
    else if (mqCall.getErrNum() == TIMEOUT_ERRNO)
    {
        return createErrorFromErrnum(ETIMEDOUT);
    }

    return cxx::success<void>();
}

cxx::error<IpcChannelError> MessageQueue::createErrorFromErrnum(const int errnum)
{
    switch (errnum)
    {
    case EACCES:
    {
        std::cerr << "access denied to message queue \"" << m_name << "\"" << std::endl;
        return cxx::error<IpcChannelError>(IpcChannelError::ACCESS_DENIED);
    }
    case EAGAIN:
    {
        std::cerr << "the message queue \"" << m_name << "\" is full" << std::endl;
        return cxx::error<IpcChannelError>(IpcChannelError::CHANNEL_FULL);
    }
    case ETIMEDOUT:
    {
        // no error message needed since this is a normal use case
        return cxx::error<IpcChannelError>(IpcChannelError::TIMEOUT);
    }
    case EEXIST:
    {
        std::cerr << "message queue \"" << m_name << "\" already exists" << std::endl;
        return cxx::error<IpcChannelError>(IpcChannelError::CHANNEL_ALREADY_EXISTS);
    }
    case EINVAL:
    {
        std::cerr << "provided invalid arguments for message queue \"" << m_name << "\"" << std::endl;
        return cxx::error<IpcChannelError>(IpcChannelError::INVALID_ARGUMENTS);
    }
    case ENOENT:
    {
        std::cerr << "message queue \"" << m_name << "\" does not exist" << std::endl;
        return cxx::error<IpcChannelError>(IpcChannelError::NO_SUCH_CHANNEL);
    }
    default:
    {
        std::cerr << "internal logic error in message queue \"" << m_name << "\" occurred" << std::endl;
        return cxx::error<IpcChannelError>(IpcChannelError::INTERNAL_LOGIC_ERROR);
    }
    }
}

} // namespace posix
} // namespace iox
