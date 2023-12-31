#include <iostream> 
#include <string>
#include <vector>
#include <fstream>
#include <ctime>
#include <thread>
#include <conio.h>
#include <sys/stat.h>
#include <dirent.h>
#include <windows.h> 
using namespace std;
string Gpath,Gpath2[100];
void sprint(string s,int time)
{
	int len=s.size();
	for(int i=0;i<len;i++)
	{
		if(s[i]=='*')
		{
			Sleep(100);
			continue;
		}
		printf("%c",s[i]);
		Sleep(time);
	}
}

inline void neglect(int x)
{
	int num;
	char ch;
	while(num<x)
	{
		ch=getchar();
		if(ch=='\n') ++num;
	}
	/*string tmp;
	int num=0;
	while(num<x)
	{
		getline(cin,tmp);
		++num;
	 } */
}

bool isDirectory(const std::string& path)
{
#ifdef _WIN32
    DWORD attr = GetFileAttributes(path.c_str());
    return (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY));
#else
    DIR* dir = opendir(path.c_str());
    if (dir)
    {
        closedir(dir);
        return true;
    }
    return false;
#endif
}

void listSubdirectories(const std::string& folderPath, std::vector<std::string>& subdirectories)
{
#ifdef _WIN32
    WIN32_FIND_DATAA findData;
    HANDLE hFind = FindFirstFileA((folderPath + "\\*").c_str(), &findData);
    if (hFind != INVALID_HANDLE_VALUE)
    {
        do
        {
            if ((findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && strcmp(findData.cFileName, ".") != 0 && strcmp(findData.cFileName, "..") != 0)
            {
                subdirectories.push_back(findData.cFileName);
            }
        } while (FindNextFileA(hFind, &findData));
        FindClose(hFind);
    }
#else
    DIR* dir = opendir(folderPath.c_str());
    if (dir)
    {
        struct dirent* entry;
        while ((entry = readdir(dir)) != nullptr)
        {
            if (entry->d_type == DT_DIR && strcmp(entry->d_name, ".") != 0 && strcmp(entry->d_name, "..") != 0)
            {
                subdirectories.push_back(entry->d_name);
            }
        }
        closedir(dir);
    }
#endif
}

std::string getModificationDate(const std::string& filePath)//Folder modification date
{
    std::string modificationDate;
#ifdef _WIN32
    WIN32_FIND_DATAA fileData;
    HANDLE hFile = FindFirstFileA(filePath.c_str(), &fileData);
    if (hFile != INVALID_HANDLE_VALUE)
    {
        FILETIME ft = fileData.ftLastWriteTime;
        SYSTEMTIME st;
        FileTimeToSystemTime(&ft, &st);

        char buffer[256];
        snprintf(buffer, sizeof(buffer), "%04d-%02d-%02d %02d:%02d:%02d",
            st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);

        modificationDate = buffer;

        FindClose(hFile);
    }
#else
    struct stat info;
    if (stat(filePath.c_str(), &info) != 0)
        return modificationDate;

    time_t t = info.st_mtime;
    struct tm* tm = localtime(&t);

    char buffer[256];
    strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", tm);

    modificationDate = buffer;
#endif

    return modificationDate;
}
string temp[100];
void ListFiles(const std::string& folderPath) {
    std::string searchPath = folderPath + "\\*.*";

    WIN32_FIND_DATAA findData;
    HANDLE hFind = FindFirstFileA(searchPath.c_str(), &findData);
	int i=0;
    if (hFind != INVALID_HANDLE_VALUE) {
        do {
            if (!(findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
            	temp[++i]=findData.cFileName;
                cout<< i << ".  " << findData.cFileName << std::endl;
            }
        } while (FindNextFileA(hFind, &findData));

        FindClose(hFind);
    }
}
void Qread()
{
	char ch;
	ch=getchar();
	while(ch!=':') ch=getchar();
	return ;
}
string GetRegistryValue(const std::string& keyPath, const std::string& valueName)
{
    HKEY hKey;
    string valueData;

    if (RegOpenKeyExA(HKEY_CURRENT_USER, keyPath.c_str(), 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
		char buffer[1024];
		DWORD dataSize = sizeof(buffer);
		if (RegGetValueA(hKey, NULL, valueName.c_str(), RRF_RT_ANY, NULL, buffer, &dataSize) == ERROR_SUCCESS) {
			valueData = buffer;
		}
		RegCloseKey(hKey);
	}
	else
	{
		printf("\n\n��������� 7-Zip ���и�ѹ�����ݣ�����ļ��������δ��װ 7-Zip �����ȵ����� 7-zip.org ���ء�\n\n");
	}
    return valueData;
}
struct names{
	string real,alias;
	int x;
}name[100];
string rname2[20],Bpath,command,yasuo,lv;
bool prebf,ontop,choice,echos;
HWND hwnd;
struct File {
    string name;
    time_t modifiedTime;
};
int PreSolve(string s)
{
	int len=s.size(),tt=0;
	for(int i=0;i<len;++i)
	{
		if(s[i]=='$')
		{
			++tt;
			continue;
		}
		Gpath2[tt]+=s[i];
	}
	return tt;
}
void Backup(int bf,bool echo)
{
	string folderName = Bpath + "/" + name[bf].alias; // Set folder name
	// Create a folder using mkdir ()
	int result = mkdir(folderName.c_str());
	time_t now = time(0);
    tm *ltm = localtime(&now);
    string com=asctime(ltm),tmp="";
    
    for(int j=0;j<com.size();++j)
    	if(j>=11 && j<=18)
    		if(j==13 || j==16) tmp+="-";
    		else tmp+=com[j];
    tmp="["+tmp+"]"+name[bf].alias;
	if(echo) command=yasuo+" a -t7z -mx="+lv+" "+tmp+" \""+name[bf].real+"\"\\*";
	else command=yasuo+" a -t7z -mx="+lv+" "+tmp+" \""+name[bf].real+"\"\\*";
	cout<< endl << command <<endl;//debug 
	system(command.c_str());
	if(echo) command="move "+tmp+".7z "+folderName;
	else command="move "+tmp+".7z "+folderName;
	system(command.c_str());
	return ;
}
void CreateConfig()
{
	printf("\n����Ҫ���� (1)һ������ ���� (2)ȫ�Զ�����\n\n");
	char ch=getch();
	string folderName,filename = "config1.ini";
	string i="1";
    ifstream file(filename);
    while(true)
    {
    	i[0]+=1;
    	filename="config"+i+".ini";
    	ifstream file(filename);
    	if(!file.is_open()) break;
	}
	if(ch=='1')
	{
		printf("\n���ڴ�����Ϊ %s �������ļ�\n",&filename[0]);     
    	ofstream newFile(filename);
    	printf("������浵�ļ��еĴ���·�� (����ļ���·������$�ָ�): ");
		getline(cin,Gpath);
		printf("�����뱸�ݴ洢�ļ���·��:");
		getline(cin,Bpath);
		for(int i=0;i<=10;++i)
			Gpath2[i]="";
		int summ=PreSolve(Gpath);
        if (newFile.is_open()) {
        	newFile << "Auto:0" << endl;
        	newFile << "���д浵·��:" << Gpath2[0];
        	if(summ>1) newFile << '$'; 
        	for(int i=1;i<summ;++i)
        		newFile << Gpath2[i] << '$';
        	if(summ!=0) newFile << Gpath2[summ] << endl;
        	else newFile << endl;
            newFile << "���ݴ洢·��:" << Bpath << endl;
			string keyPath = "Software\\7-Zip"; 
			string valueName = "Path";
			string softw=GetRegistryValue(keyPath, valueName),softww="";
			for(int i=0;i<softw.size();++i)
				if(softw[i]==' ') softww+='"',softww+=' ',softww+='"';
				else softww+=softw[i];
            newFile << "ѹ�����·��:" << softww+"7z.exe" << endl;
            newFile << "��ԭǰ�ȱ���:0" << endl;
            newFile << "�������ö�:0" << endl;
            newFile << "�ֶ�ѡ�񱸷�:0" << endl;
            newFile << "������ʾ:1" << endl;
            newFile << "ѹ���ȼ�(Խ�ߣ�ѹ����Խ�ͣ����ٶ�Խ��):5" << endl;
    	}
    	printf("\n�����´浵:\n\n"); 
    	for(int i=0;i<=summ;++i)
    	{
    		cout << endl; 
    		std::vector<std::string> subdirectories;
			listSubdirectories(Gpath2[i], subdirectories);
		    for (const auto& folderName : subdirectories)
		    {
				std::string NGpath=Gpath2[i]+"/"+folderName;
		        std::string modificationDate = getModificationDate(NGpath);
		        std::cout << "�浵����: " << folderName << endl;
		        std::cout << "�������ʱ�� " << modificationDate << endl;
		        std::cout << "-----------" << endl;
		    }
		    Sleep(2000);
		    sprint("������������Ҫ����Щ�ļ�����һ���������Լ����ı�����\n\n",50);
			for (const auto& folderName : subdirectories)
		    {
		        string alias;
		        cout << "������´浵���ñ���(������һ������) " << folderName << endl;
		        cin >> alias;
				newFile << folderName << endl << alias << endl;
		    }
		    newFile << "$" << endl;
		}
	    newFile << "*" << endl;
	    newFile.close();
	    sprint("�����ļ�������ϣ�����\n",10);
        return ;
	}
	else if(ch=='2')
	{
		ofstream newFile(filename);
		newFile << "AUTO:1" << endl;
		int configs;
		printf("��Ҫ���õ������ļ����(���л�ȡ�浵���ƺͱ���):\n");
		cin>>configs;
		newFile << "Use Config:" << configs << endl;
		printf("��Ҫ���ݵڼ����浵:");
		cin>>configs;
		newFile << "BF:" << configs << endl;
		printf("����Ҫ (1)��ʱ���� ���� (2)�������\n");
		ch=getch();
		if(ch=='1'){
			printf("������Ҫ��ʲôʱ�䱸��: 1.�������·ݣ�Ȼ��س�(����0��ʾÿ����):");
			int mon,day,hour,min;
			scanf("%d",&mon);
			printf("2.���������ڣ�Ȼ��س�(����0��ʾÿ��):");
			scanf("%d",&day);
			printf("3.������Сʱ��Ȼ��س�(����0��ʾÿСʱ):");
			scanf("%d",&hour);
			printf("4.��������ӣ�Ȼ��س�:");
			scanf("%d",&min);
			newFile << "Mode:1\nTime:" << mon << " " << day << " " << hour << " " << min << endl;
		} 
		else if(ch=='2'){
			printf("������Ҫ������ٷ��ӱ���:");
			int detime;
			scanf("%d",&detime);
			newFile << "Mode:2\nTime:" << detime << endl;
		}
		else{
			printf("\nError\n");
			return ;
		}
		printf("�Ƿ��������ģʽ(0/1):");
		cin>>configs;
		newFile << "Inter:" << configs << "\n*";
		sprint("�����ļ�������ϣ�����\n",10);
		return ;
	}
	else
	{
		printf("\n\nError\n\n");
		return ;
	}
}
void Main()
{
	string folderName;
	string filename = "config.ini";
    ifstream file(filename);
    if (!file.is_open()) {
		printf("\n���ڽ��������ļ�......\n"); 
    	ofstream newFile(filename);
    	printf("������浵�ļ��еĴ���·�� (����ļ���·������$�ָ�): ");
		getline(cin,Gpath);
		printf("������浵���ݴ洢·��:");
		getline(cin,Bpath);
		int summ=PreSolve(Gpath);
        if (newFile.is_open()) {
        	newFile << "ʹ�õ������ļ����:0" << endl;
        	/*newFile << "Backup parent folder path:" << Gpath2[0] << '$';
        	for(int i=1;i<summ;++i)
        		newFile << Gpath2[i] << '$';
        	newFile << Gpath2[summ] << endl;*/
        	newFile << "�浵�ļ���·��:" << Gpath << endl;//new
            newFile << "�浵���ݴ洢·��:" << Bpath << endl;
			string keyPath = "Software\\7-Zip"; 
			string valueName = "Path";
			string softw=GetRegistryValue(keyPath, valueName),softww="";
			for(int i=0;i<softw.size();++i)
				if(softw[i]==' ') softww+='"',softww+=' ',softww+='"';
				else softww+=softw[i];
            newFile << "ѹ�����·��:" << softww+"7z.exe" << endl;
            newFile << "����ǰ�Ȼ�ԭ:0" << endl;
            newFile << "�������ö�:0" << endl;
            newFile << "�ֶ�ѡ��ԭ(Ĭ��ѡ����):0" << endl;
            newFile << "������ʾ:1" << endl;
            newFile << "ѹ���ȼ�:5" << endl;
    	}
    	printf("\n�����´浵�ļ���:\n\n"); 
    	for(int i=0;i<=summ;++i)
    	{
    		cout << endl; 
    		std::vector<std::string> subdirectories;
			listSubdirectories(Gpath2[i], subdirectories);
		    for (const auto& folderName : subdirectories)
		    {
				std::string NGpath=Gpath2[i]+"/"+folderName;
		        std::string modificationDate = getModificationDate(NGpath);
		        std::cout << "�浵����: " << folderName << endl;
		        std::cout << "�������ʱ��: " << modificationDate << endl;
		        std::cout << "-----------" << endl;
		    }
		    Sleep(2000);
		    sprint("����������Ҫ����Щ�ļ�����һ���������Լ����ı�����\n\n",50);
			for (const auto& folderName : subdirectories)
		    {
		        string alias;
		        cout << "���������´浵�ı���(������һ������) " << folderName << endl;
		        cin >> alias;
				newFile << folderName << endl << alias << endl;
		    }
		    newFile << "$" << endl;
		}
	    newFile << "*" << endl;
	    newFile.close();
        return ;
    }
    
    
    
    freopen("config.ini","r",stdin);
    Qread();
    string temps,tempss;
    int configs;
    char configss[100];
	scanf("%d",&configs);
	if(configs!=0)
	{
		temps=to_string(configs); //only c++11
		temps="config"+temps+".ini";
		/*for(int i=0;i<temps.size();++i)
			configss[i]=temps[i];*/
		strcpy(configss,temps.c_str());
		freopen(configss,"r",stdin);
		Qread();
		int auto1;
		cin>>auto1;
		if(auto1)
		{
			int mode,usecf,bfnum,ifinter;
			Qread();
			cin>>usecf;
			Qread();
			cin>>bfnum;
			Qread();
			cin>>mode;
			int bftime,month,day,hour,min;
			if(mode==1)
			{
				Qread(); 
				scanf("%d %d %d %d",&month,&day,&hour,&min);//�������������󣬺������ȷ���� 
				//2023.12.31����������ļ������һ���������� 
			}
			else if(mode==2)
			{
				Qread();
				cin>>bftime;
			}
			Qread();
			cin>>ifinter;
			if(usecf==0) // ʹ��һ�������еĴ浵·�� 
			{
//				freopen("CON","r",stdin);
				freopen("config.ini","r",stdin);
//				getline(cin,tmp1);// Problem Here! ������������������ 
				/*for(int i=0;i<=10;++i)
					tmp[i]=getchar();*/
//				cin.getline(tmp,100);
//				cout<<"\n\n\n"<<tmp1<<"\n\n\n\n";
				neglect(1); 
				Qread();
				char inputs[1000];
				for(int i=0;;++i)
				{
					inputs[i]=getchar();
					if(inputs[i]=='\n'){
						inputs[i]='\0';break;
					}
				}
				tempss=inputs;
			    int summ=PreSolve(tempss);
			    Qread();
			    memset(inputs,'\0',sizeof(inputs));
			    for(int i=0;;++i)
				{
					inputs[i]=getchar();
					if(inputs[i]=='\n'){
						inputs[i]='\0';break;
					}
				}
				Bpath=inputs;
				Qread();
				memset(inputs,'\0',sizeof(inputs));
			    for(int i=0;;++i)
				{
					inputs[i]=getchar();
					if(inputs[i]=='\n'){
						inputs[i]='\0';break;
					}
				}
				yasuo=inputs;
				neglect(4);
				Qread();
				memset(inputs,'\0',sizeof(inputs));
				inputs[0]=getchar();
				lv=inputs;
			    int i=0;
//			    printf("Your folder is as follows:\n\n");
			    int ttt=0;
			    inputs[0]=getchar();// addition
			    while(true)
			    {
//			    	getline(cin,name[++i].real);
					memset(inputs,'\0',sizeof(inputs));
					for(int i=0;;++i)
					{
						inputs[i]=getchar();
						if(inputs[i]=='\n'){
							inputs[i]='\0';break;
						}
					}
					name[++i].real=inputs;
			    	if(name[i].real[0]=='*') break;
			    	else if(name[i].real[0]=='$')
			    	{
			    		++ttt,--i;
			    		continue;
					}
			    	name[i].real=Gpath2[ttt]+"/"+name[i].real;
			    	name[i].x=ttt;
			    	memset(inputs,'\0',sizeof(inputs));
					for(int i=0;;++i)
					{
						inputs[i]=getchar();
						if(inputs[i]=='\n'){
							inputs[i]='\0';break;
						}
					}
					name[i].alias=inputs;
				}
			}
			else
			{
				string temps=to_string(usecf);
			    char configss[10];
				temps="config"+temps+".ini";
				for(int i=0;i<temps.size();++i)
					configss[i]=temps[i];
				freopen(configss,"r",stdin);
				string tmp;
				getline(cin,tmp);
				Qread();
			    getline(cin,temps);
			    int summ=PreSolve(temps);
			    Qread();
			    getline(cin,Bpath);
			    Qread();
			    getline(cin,yasuo);
			    neglect(4);
				Qread();
				getline(cin,lv);
			    int i=0;
			    //printf("Your folder is as follows:\n\n");
			    int ttt=0;
			    while(true)
			    {
			    	getline(cin,name[++i].real);
			    	if(name[i].real[0]=='*') break;
			    	else if(name[i].real[0]=='$')
			    	{
			    		++ttt,--i;
			    		continue;
					}
			    	name[i].real=Gpath2[ttt]+"/"+name[i].real;
			    	name[i].x=ttt;
			    	getline(cin,name[i].alias);
				}
			}
			if(mode==1)
			{
				while(true)
				{
					// ��ȡ��ǰʱ��
				    std::time_t now = std::time(nullptr);
				    std::tm* local_time = std::localtime(&now);
				
				    std::tm target_time = {0};
				    target_time.tm_year = local_time->tm_year; // ��ݴ�1900�꿪ʼ����
				    if(month==0)  target_time.tm_mon = local_time->tm_mon+1;
				    else target_time.tm_mon = month - 1; // �·ݴ�0��ʼ����
				    if(day==0) target_time.tm_mday = local_time->tm_mday+1;
				    else target_time.tm_mday = day;
				    if(hour==0) target_time.tm_hour = local_time->tm_hour+1;
				    else target_time.tm_hour = hour;
				    if(min==0) target_time.tm_min = local_time->tm_min+1;
				    else target_time.tm_min = min;
//				    if(min==0 || hour==0 || day==0 || month==0) 
//				    	target_time.tm_sec = local_time->tm_sec+1;
				    // �����ǰʱ���Ѿ�������Ŀ��ʱ�䣬��ô�Ͳ���Ҫ�ȴ���
				    if (std::mktime(local_time) > std::mktime(&target_time)) {
				        std::cout << "���ڵ�ʱ���Ѿ�������Ŀ��ʱ��" << std::endl;
				    } else {
				        // ������Ҫ�ȴ���ʱ�䣨��λ���룩
				        std::time_t wait_time = std::difftime(std::mktime(&target_time), std::mktime(local_time));
				        // �ȴ�ָ����ʱ��
				        std::this_thread::sleep_for(std::chrono::seconds(wait_time));
				        Backup(bfnum,false);
				    }
				}
			} 
			else if(mode==2)
			{
				while(true)
				{
					auto now = std::chrono::steady_clock::now();
				    // ����Ŀ��ʱ�䣨���磺5���
				    auto target_time = now + std::chrono::seconds(10);
				
				    // ����ʱ���ֵ
				    auto duration = target_time - now;
				
				    // ���߳�����ָ����ʱ��
				    std::this_thread::sleep_for(duration);
				    Backup(bfnum,false);
				}
				
			}
			//freopen("","r",stdin) //NEWWW
		}
	}
    Qread();
    getline(cin,temps);
    int summ=PreSolve(temps);
    Qread();
    getline(cin,Bpath);
    Qread();
    getline(cin,yasuo);
//    yasuo="\""+yasuo+"\""; useless
    Qread();
    cin>>prebf;
    Qread();
    cin>>ontop;
    Qread();
	cin>>choice;
	Qread();
	cin>>echos;
	Qread();
	getline(cin,lv);
    int i=0;
    printf("�����´浵:\n\n");
    int ttt=0;
    //char ch=getchar();//DEBUG because getline ...//bug why?now ok?
    while(true)
    {
    	getline(cin,name[++i].real);
    	if(name[i].real[0]=='*') break;
    	else if(name[i].real[0]=='$')
    	{
    		++ttt,--i;
    		continue;
		}
    	name[i].real=Gpath2[ttt]+"/"+name[i].real;
    	name[i].x=ttt;
    	getline(cin,name[i].alias);
    	printf("%d. ",i);
    	cout<<name[i].alias<<endl;
	}
	freopen("CON","r",stdin);
	
	while(true)
	{
		printf("������Ҫ (1)���ݴ浵 (2)�ص� (3)���´浵 (4) �Զ����� ���� (5) ���������ļ� �أ� (�� 1/2/3/4/5)\n");
		char ch;
		ch=getch();
		if(ch=='1')
		{
			printf("����浵ǰ���������ɱ���:");
			int bf;
			scanf("%d",&bf);
			Backup(bf,echos);
			sprint("\n\n�������! ! !\n\n",40);
		}
		else if(ch=='2')
		{
			int i=0;
		    if(!choice) printf("����浵ǰ���������ɻ�ԭ (ģʽ: ѡȡ���±���) : ");
		    else printf("����浵ǰ���������ɻ�ԭ (ģʽ: �ֶ�ѡ��) :");
			int bf;
			scanf("%d",&bf);
			string folderPath=Bpath+"/"+name[bf].alias+"/";
			DIR* directory = opendir(folderPath.c_str());
		    if (!directory) {
		        printf("���ݲ����ڣ��޷���ԭ������Ի�û�н��й�����\n");
		        return ;
		    }
		    File files;
		    if(!choice)//Look for the latest backup
		    {
			    struct dirent* entry;
			    while ((entry = readdir(directory))) {
			        string fileName = entry->d_name;
			        string filePath = folderPath + fileName;
			        struct stat fileStat;
			        if (stat(filePath.c_str(), &fileStat) != -1) {
			            if (S_ISREG(fileStat.st_mode)) { // Only regular files are processed
			                File file;
			                file.name = fileName;
			                file.modifiedTime = fileStat.st_mtime;
							if(file.modifiedTime>files.modifiedTime)
							{
								files.modifiedTime=file.modifiedTime;
								files.name=file.name;
							}
								
			            }
			        }
			    }
			    closedir(directory);
			}
			else
			{
				string folderName2 = Bpath + "/" + name[bf].alias;
				printf("�����Ǳ��ݴ浵\n\n");
				ListFiles(folderName2);
				printf("���뱸��ǰ���������ɻ�ԭ:");
				int bf2;
				scanf("%d",&bf2);
				files.name=temp[bf2];
			}
		    if(prebf)
		    	Backup(bf,false);
			command=yasuo+" x "+Bpath+"/"+name[bf].alias+"/"+files.name+" -o"+name[bf].real+" -y";
			system(command.c_str());
			sprint("\n\n��ԭ�ɹ�! ! !\n\n",40);
		}
		else if(ch=='3')
		{
			freopen("CON","r",stdin);
        	ofstream cfile("config.ini");
        	cfile << "ʹ�õ������ļ����:0\n";
        	cfile << "�浵�ļ���·��:" << Gpath2[0] << '$';
        	for(int i=1;i<summ;++i)
        		cfile << Gpath2[i] << '$';
        	cfile << Gpath2[summ] << endl;
        	cfile << "�浵���ݴ洢·��:" << Bpath << endl;
			string keyPath = "Software\\7-Zip"; 
			string valueName = "Path";
            cfile << "ѹ�����·��:" << GetRegistryValue(keyPath, valueName)+"7z.exe" << endl;
            cfile << "����ǰ��ԭ:" << prebf << endl;
            cfile << "�������ö�:" << ontop << endl;
            cfile << "�ֶ�ѡ�񱸷�:" << choice << endl;
            cfile << "������ʾ:" << echos << endl;
            cfile << "ѹ���ȼ�:" << lv << endl;
        	printf("\n���ļ����������´浵: \n\n"); 
	    	for(int i=0;i<=summ;++i)
	    	{
	    		vector<string> subdirectories;
				listSubdirectories(Gpath2[i], subdirectories);
			    for (const auto& folderName : subdirectories)
			    {
					string NGpath=Gpath2[i]+"/"+folderName;
			        string modificationDate = getModificationDate(NGpath);
			        cout << "�浵����: " << folderName << endl;
			        cout << "�������ʱ��: " << modificationDate << endl;
			        cout << "-----------" << endl;
			    }
			    Sleep(2000);
			    sprint("������������Ҫ����Щ�ļ�����һ���������Լ����ı�����\n\n",50);
				for (const auto& folderName : subdirectories)
			    {
			        string alias;
			        cout << "���������´浵�ı���(������һ������) " << endl << folderName;
			        cin >> alias;
					cfile << folderName << endl << alias << endl;
			    }
			    cfile << "$" << endl;
			}
		    cfile << "*" << endl;
	    	puts("\n\n���´浵���\n\n");
	    	cfile.close(); 
		}
		else if(ch=='4')
		{
			printf("��������Ҫ���ݵĴ浵���:");
			int bf,tim;
			scanf("%d",&bf);
			printf("ÿ�������ӽ��б���: ");
			scanf("%d",&tim);
			printf("�ѽ����Զ�����ģʽ��ÿ�� %d ���ӽ��б���",tim);
			while(true)
			{
				Backup(bf,false);
				Sleep(tim*60000);
			}
		}
		else if(ch=='5')
		{
			CreateConfig();
		}
		else printf("�밴�����ϵ� 1/2/3/4/5 ��\n\n");
	}
	std::this_thread::sleep_for(std::chrono::seconds(1));
//    return ;
}

bool isRunning = true;
LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    switch (uMsg)
    {
    case WM_COMMAND:
    {
        int buttonId = LOWORD(wParam);

        // Handle button click events
        switch (buttonId)
        {
	        case 1:
	            command="start "+Gpath2[0];
				system(command.c_str());
	            break;
	        case 2:
	            command="start "+Bpath;
				system(command.c_str());
	            break;
	        case 3:
	            command="start config.ini";
				system(command.c_str());
	            break;
		}
		break;
	}
	case WM_DESTROY:
		PostQuitMessage(0);
		return 0;
	}
    return DefWindowProc(hwnd, uMsg, wParam, lParam);
}
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow)
{
	HWND h=GetForegroundWindow();
    WNDCLASS wc = {};
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = "ButtonWindowClass";
    RegisterClass(&wc);

    HWND hwnd = CreateWindowEx(
        0,                          
        wc.lpszClassName,          
        "Toolbox",                  
        WS_OVERLAPPEDWINDOW,          
        CW_USEDEFAULT, CW_USEDEFAULT, 
        480, 85,              
        NULL,                     
        NULL,                     
        hInstance,
        NULL
    );
    CreateWindow("button", "�򿪴浵�ļ���",
        WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON,
        20, 10, 120, 35,
        hwnd, (HMENU)1, hInstance, NULL);

    CreateWindow("button", "�򿪱����ļ���",
        WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON,
        150, 10, 120, 35,
        hwnd, (HMENU)2, hInstance, NULL);

    CreateWindow("button", "����",
        WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON,
        280, 10, 80, 35,
        hwnd, (HMENU)3, hInstance, NULL);

    ShowWindow(hwnd, nCmdShow);
    if(ontop) SetWindowPos(h,HWND_TOPMOST,0,0,0,0,SWP_NOMOVE | SWP_NOSIZE);//Top the window
    std::thread MainThread(Main);
    MSG msg = {};
    while (GetMessage(&msg, NULL, 0, 0))
    {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    isRunning = false;
    MainThread.join();
    return 0;
}
