#pragma once

struct AppConfig;

class ConsoleGCSApplication
{
public:
    int Run(int argc, char* argv[]);

private:
    int RunReadOnlyMavlink(const AppConfig& appConfig);
    int RunLegacySingleSession(const AppConfig& appConfig);
};
