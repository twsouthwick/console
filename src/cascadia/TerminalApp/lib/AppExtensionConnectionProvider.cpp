#include "pch.h"
#include "AppConnectionProvider.h"

#include "winrt/Windows.ApplicationModel.h"
#include "winrt/Windows.Storage.h"
#include "winrt/Windows.Storage.Streams.h"
#include "winrt/Windows.Foundation.Collections.h"
#include "winrt/Windows.Storage.Search.h"
#include "winrt/Windows.ApplicationModel.AppExtensions.h"
#include "winrt/Windows.ApplicationModel.AppService.h"

#include <thread>

using namespace winrt;
using namespace Windows::Foundation;
using namespace winrt::Microsoft::Terminal::TerminalConnection;
using namespace winrt::Windows::Foundation::Collections;
using namespace winrt::Windows::ApplicationModel::AppExtensions;
using namespace winrt::Windows::ApplicationModel::AppService;

struct AppExtensionTerminalConnection : implements<AppExtensionTerminalConnection, ITerminalConnection>
{
    AppExtensionTerminalConnection(AppExtension extension, hstring serviceName, TerminalConnectionStartupInfo startupInfo) :
        _connection(),
        _extension(extension),
        _startupInfo(startupInfo),
        _status(AppServiceConnectionStatus::Unknown)
    {
        _connection.AppServiceName(serviceName);
        _connection.PackageFamilyName(extension.Package().Id().FamilyName());

        auto name = _connection.PackageFamilyName();

        _connection.RequestReceived([this](auto&& connection, AppServiceRequestReceivedEventArgs args) {
            auto request = args.Request();
            auto message = request.Message();

            if (!message.HasKey(L"command"))
            {
                UnknownError(L"No command value available");
                return;
            }

            auto command = message.Lookup(L"command").as<IPropertyValue>().GetString();

            if (message.HasKey(L"output"))
            {
                auto value = message.Lookup(L"value").try_as<IPropertyValue>();

                if (value != nullptr)
                {
                    _outputHandlers(value.GetString());
                }
                else
                {
                    UnknownError(L"No value key for output command");
                }
            }
            else if (message.HasKey(L"disconnect"))
            {
                _disconnectHandlers();
            }
            else
            {
                UnknownError(L"");
            }
        });
    }

    event_token TerminalOutput(TerminalOutputEventArgs const& handler)
    {
        return _outputHandlers.add(handler);
    }

    void TerminalOutput(winrt::event_token const& token) noexcept
    {
        _outputHandlers.remove(token);
    }

    event_token TerminalDisconnected(TerminalDisconnectedEventArgs const& handler)
    {
        return _disconnectHandlers.add(handler);
    }

    void TerminalDisconnected(event_token const& token) noexcept
    {
        _disconnectHandlers.remove(token);
    }

    void Start()
    {
        _connection.OpenAsync().Completed([this](auto&& result, AsyncStatus status) {
            _status = result.get();

            if (_status != AppServiceConnectionStatus::Success)
            {
                UnknownError(L"Could not connect to service");
            }
        });
    }

    void WriteInput(hstring const& data)
    {
        if (_status == AppServiceConnectionStatus::Success)
        {
            ValueSet set;
            set.Insert(L"command", box_value(L"input"));
            set.Insert(L"value", box_value(data));
            _connection.SendMessageAsync(set).Completed([](auto&& sender, AsyncStatus status) {
                OutputDebugString(L"Input sent\n");
            });
        }
    }

    void Resize(uint32_t rows, uint32_t columns)
    {
        if (_status == AppServiceConnectionStatus::Success)
        {
            ValueSet set;
            set.Insert(L"command", box_value(L"resize"));
            set.Insert(L"rows", box_value(rows));
            set.Insert(L"columns", box_value(columns));
            _connection.SendMessageAsync(set).Completed([](auto&& sender, AsyncStatus status) {
                OutputDebugString(L"Resize sent\n");
            });
        }
    }

    void Close()
    {
        if (_status == AppServiceConnectionStatus::Success)
        {
            ValueSet set;
            set.Insert(L"command", box_value(L"resize"));
            _connection.SendMessageAsync(set).Completed([](auto&& sender, AsyncStatus status) {
                OutputDebugString(L"Close sent\n");
            });
        }
    }

private:
    AppExtension _extension;
    AppServiceConnection _connection;
    TerminalConnectionStartupInfo _startupInfo;

    event<TerminalOutputEventArgs> _outputHandlers;
    event<TerminalDisconnectedEventArgs> _disconnectHandlers;

    AppServiceConnectionStatus _status;

    void UnknownError(hstring msg)
    {
        _status = AppServiceConnectionStatus::AppServiceUnavailable;
        _disconnectHandlers();
    }
};

struct AppExtensionFactory : implements<AppExtensionFactory, ITerminalConnectionFactory>
{
    AppExtensionFactory(AppExtension extension, hstring name, hstring serviceName, hstring cmdline, guid connectionType) :
        _extension(extension),
        _name(name),
        _serviceName(serviceName),
        _cmdline(cmdline),
        _connectionType(connectionType)
    {
    }

    hstring Name()
    {
        return _name;
    }

    hstring CmdLine()
    {
        return _cmdline;
    }

    Uri IconUri()
    {
        return nullptr;
    }

    guid ConnectionType()
    {
        return _connectionType;
    }

    ITerminalConnection Create(TerminalConnectionStartupInfo startupInfo)
    {
        return make<AppExtensionTerminalConnection>(_extension, _serviceName, startupInfo);
    }

    static ITerminalConnectionFactory Load(AppExtension extension, IPropertySet properties)
    {
        auto name = extension.DisplayName();

        if (name.empty())
        {
            return nullptr;
        }

        auto cmdline = GetText(properties, L"cmdline");
        auto service = GetText(properties, L"Service");

        // Get id
        GUID id;
        if (UuidFromString((RPC_WSTR)extension.Id().data(), &id) != RPC_S_OK)
        {
            return nullptr;
        }

        return make<AppExtensionFactory>(extension, name, service, cmdline, guid(id));
    }

private:
    static hstring GetText(IPropertySet set, hstring name)
    {
        auto subset = set.TryLookup(name).try_as<IPropertySet>();

        if (subset != nullptr)
        {
            auto text = subset.TryLookup(L"#text");

            if (text != nullptr)
            {
                auto value = text.try_as<IPropertyValue>();

                if (value != nullptr)
                {
                    return value.GetString();
                }
            }
        }

        return L"";
    }

    hstring _name;
    hstring _cmdline;
    hstring _serviceName;
    guid _connectionType;
    AppExtension _extension;
};

struct AppExtensionConnectionProviderAggregator : implements<AppExtensionConnectionProviderAggregator, ITerminalConnectionProvider>
{
    AppExtensionConnectionProviderAggregator(hstring extensionName) :
        _catalog(AppExtensionCatalog::Open(extensionName)),
        _factories()
    {
        _factories = LoadFactoriesFromAppExtensions();
    }

    ITerminalConnectionFactory GetFactory(guid id)
    {
        for (auto factory : _factories)
        {
            if (factory.ConnectionType() == id)
            {
                return factory;
            }
        }

        return nullptr;
    }

    com_array<ITerminalConnectionFactory> GetFactories()
    {
        return com_array<ITerminalConnectionFactory>(_factories);
    }

private:
    std::vector<ITerminalConnectionFactory> _factories;

    AppExtensionCatalog _catalog;
    std::vector<ITerminalConnectionFactory> LoadFactoriesFromAppExtensions()
    {
        std::vector<ITerminalConnectionFactory> result;

        auto extensions = _catalog.FindAllAsync().get();

        for (auto extension : extensions)
        {
            auto properties = extension.GetExtensionPropertiesAsync().get();
            auto loaded = AppExtensionFactory::Load(extension, properties);

            if (loaded != nullptr)
            {
                result.push_back(loaded);
            }
        }

        return result;
    }
};

ITerminalConnectionProvider GetTerminalConnectionProvider()
{
    return make<AppExtensionConnectionProviderAggregator>(L"com.microsoft.terminal.connection");
}
