// main/langstudy_builtin.c
// Built-in CET-4/CET-6 pack, format version 2. English fields stay ASCII; the
// translation field is UTF-8 and is rendered with the generated CJK font.
//
// Keep this list stable: progress records key on these word IDs, so editing a
// word's text without changing its ID is harmless (the store remaps by text),
// but deleting an ID silently drops that word's history.
#include "langstudy_builtin.h"

#include "langstudy_content.h"

#include <stdlib.h>
#include <string.h>

// clang-format off
static const char s_builtin_body[] =
    "LSPACK|2|10|100\n"
    "LESSON|1|Campus and Study\n"
    "LESSON|2|Science and Technology\n"
    "LESSON|3|Environment and Nature\n"
    "LESSON|4|Society and Culture\n"
    "LESSON|5|Economy and Work\n"
    "LESSON|6|Health and Mind\n"
    "LESSON|7|Travel and Transport\n"
    "LESSON|8|Communication and Media\n"
    "LESSON|9|Law and Ethics\n"
    "LESSON|10|Academic Thinking\n"

    "WORD|1001|1|academic|adj.|relating to study and schools|学术的|She has a strong academic record.|AE K AH D 'EH M IH K\n"
    "WORD|1002|1|curriculum|n.|the subjects taught in a school|课程|The school updated its curriculum.|K AH R 'IH K Y AH L AH M\n"
    "WORD|1003|1|scholarship|n.|money given to help a student study|奖学金|He won a scholarship to college.|S K 'AA L ER SH IH P\n"
    "WORD|1004|1|lecture|n.|a talk given to teach students|讲座|The lecture lasted one hour.|L 'EH K CH ER\n"
    "WORD|1005|1|assignment|n.|work given to a student|作业|Finish the assignment before Friday.|AH S 'AY N M AH N T\n"
    "WORD|1006|1|discipline|n.|training that makes people obey rules|纪律|Good discipline helps students learn.|D 'IH S AH P L IH N\n"
    "WORD|1007|1|graduate|v.|to finish a course of study|毕业|She will graduate next June.|G R 'AE JH UW EY T\n"
    "WORD|1008|1|tuition|n.|money paid for teaching|学费|Tuition is higher this year.|T UW 'IH SH AH N\n"
    "WORD|1009|1|seminar|n.|a small class for discussion|研讨会|The seminar meets every Tuesday.|S 'EH M AH N AA R\n"
    "WORD|1010|1|syllabus|n.|a plan of what a course covers|教学大纲|Read the syllabus before class.|S 'IH L AH B AH S\n"

    "WORD|1011|2|technology|n.|the use of science in industry|技术|Technology changes our daily life.|T EH K N 'AA L AH JH IY\n"
    "WORD|1012|2|innovation|n.|a new idea or method|创新|The team is known for innovation.|IH N AH V 'EY SH AH N\n"
    "WORD|1013|2|experiment|n.|a test to discover something|实验|The experiment gave clear results.|IH K S P 'EH R AH M AH N T\n"
    "WORD|1014|2|hypothesis|n.|an idea to be tested|假设|The data supports her hypothesis.|HH AY P 'AA TH AH S IH S\n"
    "WORD|1015|2|research|n.|careful study of a subject|研究|His research focuses on clean energy.|R IY S 'ER CH\n"
    "WORD|1016|2|artificial|adj.|made by people, not nature|人工的|The room has artificial light.|AA R T AH F 'IH SH AH L\n"
    "WORD|1017|2|algorithm|n.|steps a computer follows|算法|The algorithm sorts the list quickly.|'AE L G ER IH DH AH M\n"
    "WORD|1018|2|digital|adj.|using computer signals|数字的|We keep digital copies of the files.|D 'IH JH AH T AH L\n"
    "WORD|1019|2|device|n.|a machine made for a purpose|设备|This device measures air quality.|D IH V 'AY S\n"
    "WORD|1020|2|efficiency|n.|working well without waste|效率|The new engine improves efficiency.|IH F 'IH SH AH N S IY\n"

    "WORD|1021|3|environment|n.|the natural world around us|环境|We must protect the environment.|IH N V 'AY R AH N M AH N T\n"
    "WORD|1022|3|pollution|n.|harmful waste in air or water|污染|The city works to reduce pollution.|P AH L 'UW SH AH N\n"
    "WORD|1023|3|sustainable|adj.|able to continue without damage|可持续的|They support sustainable farming.|S AH S T 'EY N AH B AH L\n"
    "WORD|1024|3|ecosystem|n.|living things and their habitat|生态系统|The lake is a fragile ecosystem.|'IY K OW S IH S T AH M\n"
    "WORD|1025|3|resource|n.|something useful that we use|资源|Water is a precious resource.|R IY S 'AO R S\n"
    "WORD|1026|3|conservation|n.|the protection of nature|保护|Conservation groups planted trees.|K AA N S ER V 'EY SH AH N\n"
    "WORD|1027|3|emission|n.|gas or waste sent into the air|排放|The law limits carbon emission.|IH M 'IH SH AH N\n"
    "WORD|1028|3|renewable|adj.|able to be replaced by nature|可再生的|Solar power is renewable energy.|R IH N 'UW AH B AH L\n"
    "WORD|1029|3|climate|n.|the usual weather of a region|气候|The climate here is mild.|K L 'AY M AH T\n"
    "WORD|1030|3|species|n.|a group of similar living things|物种|Many species live in this forest.|S P 'IY SH IY Z\n"

    "WORD|1031|4|culture|n.|the customs of a group of people|文化|Chinese culture has a long history.|K 'AH L CH ER\n"
    "WORD|1032|4|tradition|n.|a custom passed down over years|传统|The festival is an old tradition.|T R AH D 'IH SH AH N\n"
    "WORD|1033|4|community|n.|people living in the same area|社区|The community built a new park.|K AH M Y 'UW N AH T IY\n"
    "WORD|1034|4|generation|n.|all people born around one time|一代|Each generation faces new choices.|JH EH N ER 'EY SH AH N\n"
    "WORD|1035|4|diversity|n.|the state of being different|多样性|The city is proud of its diversity.|D AH V 'ER S AH T IY\n"
    "WORD|1036|4|heritage|n.|traditions and buildings from the past|遗产|The old town is a national heritage.|HH 'EH R AH T IH JH\n"
    "WORD|1037|4|identity|n.|who a person or group is|身份|Language shapes our identity.|AY D 'EH N T AH T IY\n"
    "WORD|1038|4|custom|n.|a usual way of behaving|习俗|It is a custom to give gifts.|K 'AH S T AH M\n"
    "WORD|1039|4|urban|adj.|relating to a city|城市的|Urban life is busy and fast.|'ER B AH N\n"
    "WORD|1040|4|migration|n.|movement from one place to another|迁移|Bird migration begins in autumn.|M AY G R 'EY SH AH N\n"

    "WORD|1041|5|economy|n.|the wealth and trade of a country|经济|The economy is growing slowly.|IH K 'AA N AH M IY\n"
    "WORD|1042|5|industry|n.|the making of goods in factories|工业|The car industry employs many people.|'IH N D AH S T R IY\n"
    "WORD|1043|5|enterprise|n.|a company or a business|企业|She started a small enterprise.|'EH N T ER P R AY Z\n"
    "WORD|1044|5|investment|n.|money put into a business|投资|The project needs foreign investment.|IH N V 'EH S T M AH N T\n"
    "WORD|1045|5|employment|n.|the state of having a paid job|就业|The program creates employment.|IH M P L 'OY M AH N T\n"
    "WORD|1046|5|consumer|n.|a person who buys things|消费者|The consumer wants lower prices.|K AH N S 'UW M ER\n"
    "WORD|1047|5|finance|n.|the management of money|金融|He works in finance.|F 'AY N AE N S\n"
    "WORD|1048|5|productive|adj.|producing good results|高效的|We had a productive meeting.|P R AH D 'AH K T IH V\n"
    "WORD|1049|5|strategy|n.|a plan to reach a goal|策略|The company changed its strategy.|S T R 'AE T AH JH IY\n"
    "WORD|1050|5|negotiation|n.|discussion to reach an agreement|谈判|The negotiation lasted two days.|N IH G OW SH IY 'EY SH AH N\n"

    "WORD|1051|6|nutrition|n.|the food your body needs|营养|Good nutrition helps children grow.|N UW T R 'IH SH AH N\n"
    "WORD|1052|6|exercise|n.|physical activity for health|锻炼|Daily exercise keeps you strong.|'EH K S ER S AY Z\n"
    "WORD|1053|6|anxiety|n.|a feeling of worry or fear|焦虑|Exams often cause anxiety.|AE NG Z 'AY AH T IY\n"
    "WORD|1054|6|depression|n.|a long feeling of deep sadness|抑郁|Talking can help with depression.|D IH P R 'EH SH AH N\n"
    "WORD|1055|6|immune|adj.|protected against a disease|免疫的|Sleep supports the immune system.|IH M Y 'UW N\n"
    "WORD|1056|6|therapy|n.|treatment for a health problem|治疗|She started therapy last month.|TH 'EH R AH P IY\n"
    "WORD|1057|6|mental|adj.|relating to the mind|心理的|Mental health matters as much.|M 'EH N T AH L\n"
    "WORD|1058|6|recover|v.|to become healthy again|恢复|He is recovering after surgery.|R IH K 'AH V ER\n"
    "WORD|1059|6|balance|n.|a steady and equal state|平衡|Find a balance between work and rest.|B 'AE L AH N S\n"
    "WORD|1060|6|awareness|n.|knowledge of a situation|意识|The campaign raised public awareness.|AH W 'EH R N AH S\n"

    "WORD|1061|7|destination|n.|the place you are traveling to|目的地|Our destination is a small town.|D EH S T AH N 'EY SH AH N\n"
    "WORD|1062|7|journey|n.|a trip from one place to another|旅程|The journey took six hours.|JH 'ER N IY\n"
    "WORD|1063|7|transport|n.|the movement of people or goods|交通|Public transport is cheap here.|T R 'AE N S P AO R T\n"
    "WORD|1064|7|vehicle|n.|a machine that carries people|车辆|No vehicle is allowed on this path.|V 'IY AH K AH L\n"
    "WORD|1065|7|accommodation|n.|a place to stay while traveling|住宿|We found cheap accommodation online.|AH K AA M AH D 'EY SH AH N\n"
    "WORD|1066|7|itinerary|n.|a plan for a trip|行程|The itinerary includes three cities.|AY T 'IH N ER EH R IY\n"
    "WORD|1067|7|departure|n.|the act of leaving a place|出发|Check the departure time again.|D IH P 'AA R CH ER\n"
    "WORD|1068|7|arrival|n.|the act of reaching a place|到达|Our arrival was delayed by rain.|AH R 'AY V AH L\n"
    "WORD|1069|7|tourism|n.|the business of traveling for fun|旅游|Tourism supports the local economy.|T 'UH R IH Z AH M\n"
    "WORD|1070|7|explore|v.|to travel to learn about a place|探索|We explored the old city on foot.|IH K S P L 'AO R\n"

    "WORD|1071|8|communication|n.|the exchange of information|交流|Clear communication prevents mistakes.|K AH M Y UW N AH K 'EY SH AH N\n"
    "WORD|1072|8|information|n.|facts or details about something|信息|The website gives useful information.|IH N F ER M 'EY SH AH N\n"
    "WORD|1073|8|broadcast|v.|to send out a program by radio or TV|广播|The interview will broadcast tonight.|B R 'AO D K AE S T\n"
    "WORD|1074|8|advertisement|n.|a public notice to sell something|广告|The advertisement appeared online.|AE D V ER T 'AY Z M AH N T\n"
    "WORD|1075|8|journalism|n.|the work of reporting the news|新闻业|She studied journalism at college.|JH 'ER N AH L IH Z AH M\n"
    "WORD|1076|8|audience|n.|people who watch or listen|观众|The audience asked many questions.|'AO D IY AH N S\n"
    "WORD|1077|8|influence|n.|the power to change someone|影响|Parents have a strong influence.|'IH N F L UW AH N S\n"
    "WORD|1078|8|persuade|v.|to make someone agree|说服|He persuaded me to join the club.|P ER S W 'EY D\n"
    "WORD|1079|8|message|n.|information sent to someone|消息|I left a message for her.|M 'EH S IH JH\n"
    "WORD|1080|8|network|n.|people or machines that are connected|网络|The school has a fast network.|N 'EH T W ER K\n"

    "WORD|1081|9|justice|n.|fair treatment under the law|正义|The victims asked for justice.|JH 'AH S T IH S\n"
    "WORD|1082|9|legal|adj.|allowed by the law|合法的|The contract is legal and binding.|L 'IY G AH L\n"
    "WORD|1083|9|regulation|n.|an official rule|法规|A new regulation limits noise.|R EH G Y AH L 'EY SH AH N\n"
    "WORD|1084|9|responsibility|n.|a duty that you must do|责任|Safety is everyone's responsibility.|R IH S P AA N S AH B 'IH L AH T IY\n"
    "WORD|1085|9|ethical|adj.|morally right|道德的|The company made an ethical choice.|'EH TH IH K AH L\n"
    "WORD|1086|9|authority|n.|the power to give orders|权威|The local authority approved the plan.|AH TH 'AO R AH T IY\n"
    "WORD|1087|9|evidence|n.|facts that show what is true|证据|The police found new evidence.|'EH V AH D AH N S\n"
    "WORD|1088|9|consequence|n.|a result of an action|后果|Every choice has a consequence.|K 'AA N S AH K W EH N S\n"
    "WORD|1089|9|principle|n.|a basic rule or belief|原则|He acted on a clear principle.|P R 'IH N S AH P AH L\n"
    "WORD|1090|9|obligation|n.|something you must do|义务|We have an obligation to help.|AA B L AH G 'EY SH AH N\n"

    "WORD|1091|10|analysis|n.|a careful study of the details|分析|Her analysis of the data was clear.|AH N 'AE L AH S IH S\n"
    "WORD|1092|10|argument|n.|a reason given to support an idea|论点|The argument is easy to follow.|'AA R G Y AH M AH N T\n"
    "WORD|1093|10|conclusion|n.|a judgment reached after study|结论|The report reached one conclusion.|K AH N K L 'UW ZH AH N\n"
    "WORD|1094|10|criterion|n.|a standard used for judging|标准|Cost is not the only criterion.|K R AY T 'IH R IY AH N\n"
    "WORD|1095|10|perspective|n.|a way of looking at something|视角|Try to see it from her perspective.|P ER S P 'EH K T IH V\n"
    "WORD|1096|10|significance|n.|the importance of something|意义|The discovery has great significance.|S IH G N 'IH F IH K AH N S\n"
    "WORD|1097|10|evaluate|v.|to judge the value of something|评估|We evaluate each plan carefully.|IH V 'AE L Y UW EY T\n"
    "WORD|1098|10|interpret|v.|to explain the meaning of something|解释|Different readers interpret the poem.|IH N T 'ER P R AH T\n"
    "WORD|1099|10|objective|adj.|based on facts, not feelings|客观的|A judge must stay objective.|AH B JH 'EH K T IH V\n"
    "WORD|1100|10|summarize|v.|to give the main points briefly|总结|Please summarize the article.|S 'AH M ER AY Z\n";
// clang-format on

const char *ls_builtin_pack_body(size_t *out_len)
{
    if (out_len) *out_len = sizeof(s_builtin_body) - 1;
    return s_builtin_body;
}

bool ls_builtin_load(ls_pack_t *out)
{
    if (!out) return false;

    size_t body_len = sizeof(s_builtin_body) - 1;
    if (body_len == 0) return false;

    // Room for one END|<8 hex> line plus the NUL terminator.
    size_t cap = body_len + 16;
    char *buf = (char *)malloc(cap);
    if (!buf) return false;

    memcpy(buf, s_builtin_body, body_len);
    bool ok = ls_pack_finalize(buf, &body_len, cap) &&
              ls_pack_parse(buf, body_len, out) == LS_OK;
    free(buf);
    return ok;
}
