/**
 * @brief ConsoleGCS 可执行程序的最小入口。
 *
 * 分类：Production；仅把命令行参数交给 ConsoleGCSApplication，不承载业务逻辑。
 */
#include "ConsoleGCSApplication.h"

int main(int argc, char* argv[])
{
    ConsoleGCSApplication application;
    return application.Run(argc, argv);
}

