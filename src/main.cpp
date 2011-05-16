#include <libintl.h>
#include <iostream>
#include <vector>
#include <stdint.h>

#include "clibase.h"

using namespace std;








int main()
{
    bindtextdomain("graphserv", "./messages");
    textdomain("graphserv");

    cout << _("Hello world!") << endl;
    return 0;
}
