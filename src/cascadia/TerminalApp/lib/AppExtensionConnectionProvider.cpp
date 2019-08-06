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
        _startupInfo(startupInfo)
    {
        _connection.AppServiceName(serviceName);
        _connection.PackageFamilyName(extension.Package().Id().FamilyName());
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
        _connection.OpenAsync().Completed([this](auto&& async, AsyncStatus status) {
            _disconnectHandlers();
        });
    }

    void WriteInput(hstring const& data)
    {
    }

    void Resize(uint32_t rows, uint32_t columns)
    {
    }

    void Close()
    {
    }

private:
    AppExtension _extension;
    AppServiceConnection _connection;
    TerminalConnectionStartupInfo _startupInfo;

    event<TerminalOutputEventArgs> _outputHandlers;
    event<TerminalDisconnectedEventArgs> _disconnectHandlers;
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
        if (CLSIDFromString(extension.Id().data(), &id) != S_OK)
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
