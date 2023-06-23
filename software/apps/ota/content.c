#include "content.h"

#define HTML_PAGE_START "<html>\n"\
"    <head>\n"\
"        <style>\n"\
"            .btn {\n"\
"                padding: 0.75rem 1.25rem;\n"\
"                font-weight: 700;\n"\
"                color: #333;\n"\
"                background-color: #fff;\n"\
"                border: 0.05rem solid #fff;\n"\
"                cursor: pointer;\n"\
"                font-size: 1.25rem;\n"\
"                border-radius: 0.3rem;\n"\
"                text-decoration: none;\n"\
"            }\n"\
"        </style>\n"\
"        <title>PicoDVI-N64</title>\n"\
"    </head>\n"\
"    <body style=\"text-align: center; background-color: #333; color: #fff; padding-top: 50px;\">\n"\
"        <div>\n"\
"            <main>"

#define HTML_PAGE_END "</main>\n"\
"        </div>\n"\
"    </body>\n"\
"</html>"

const char* HTTP_RESPONSE_HEADERS = "HTTP/1.1 %s\n"
                                    "Content-Length: %d\n"
                                    "Content-Type: text/html; charset=utf-8\n"
                                    "Connection: close\n\n";

const char* LOGO_SVG  = "<svg style=height:300px viewBox=\"982.5 734.5 1011.2 1082.1\">"\
                            "<g stroke=#abbaf3 stroke-width=90 fill=none stroke-linejoin=round stroke-miterlimit=16>"\
                                "<path d=\"m1770.1 1525.8 107.8 68.6-319 177.2v-425.2l389.8-248v354.3l-119-75.8M1559 902V779.5l318.9 177.2-389.8 248-389.7-248 318.9-177.2V898M1133.5 1385.3l-106 67.4v-354.3l389.8 248v425.2l-319-177.2 118-75\"transform=\"matrix(1 0 0 -1 0 2551)\"/>"\
                                "<g stroke=#333 stroke-width=120 transform=\"matrix(1 0 0 -1 0 2551)\">"\
                                    "<path d=\"m1311.8 1458.6 176.3-112.2 201.5 128.2\"/>"\
                                    "<path d=\"M1417.3 1020.3v184.4l-223.5 142.2\"/>"\
                                    "<path d=\"M1781.5 1346.3 1559 1204.7v-204.4\"/>"\
                                "</g>"\
                                "<g transform=\"matrix(1 0 0 -1 0 2551)\">"\
                                    "<path d=\"m1202.71 1528.05 285.39-181.65 296.1 188.37\"/>"\
                                    "<path d=\"M1417.3 888v316.7l-297.99 189.525\"/>"\
                                    "<path d=\"M1842.13 1384.91 1559 1204.7V892\"/>"\
                                "</g>"\
                            "</g>"\
                        "</svg>";

const char* OTA_BODY = HTML_PAGE_START\
"    %s"\
"    <h2>PicoDVI-N64</h2>"\
"    <h4>Installed firmware: %s (%s)</h4>"\
"    <form method=\"POST\" action=\"" OTA_PATH "\" enctype=\"multipart/form-data\">"\
"        <input type=\"file\" name=\"firmware\"/>"\
"        <input type=\"submit\" value=\"Upgrade\" class=\"btn\" />"\
"    </form>"\
"    <form method=\"POST\" action=\"" BOOT_PATH "\">"\
"        <input type=\"submit\" value=\"Boot\" class=\"btn\" />"\
"    </form>" HTML_PAGE_END;

const char* OTA_BODY_SUCCESS = HTML_PAGE_START\
"    %s"\
"    <h2>PicoDVI-N64</h2>"\
"    <h4>Upgrade successful!</h4>"\
"    <a href=\"" OTA_PATH "\" class=\"btn\">Back</a>" HTML_PAGE_END;

const char* OTA_BODY_FAILURE = HTML_PAGE_START\
"    %s"\
"    <h2>PicoDVI-N64</h2>"\
"    <h4>Upgrade failed!</h4>"\
"    <a href=\"" OTA_PATH "\" class=\"btn\">Back</a>" HTML_PAGE_END;
