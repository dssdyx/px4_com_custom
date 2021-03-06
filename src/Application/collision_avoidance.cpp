
#include <ros/ros.h>

//topic 头文件
#include <iostream>
#include <px4_command/command.h>
#include <std_msgs/Bool.h>
#include <geometry_msgs/Pose.h>
#include <geometry_msgs/PoseStamped.h>
#include <sensor_msgs/LaserScan.h>
#include <cmath>
#include <stdlib.h>


/*
 * 主要功能:
 * 1.获取激光雷达数据
 * 2.根据距离判断是否启用避障策略
 * 3.如启用避障策略,产生控制指令
 *
 */

using namespace std;

//>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>全 局 变 量<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<
enum Command
{
    Move_ENU,
    Move_Body,
    Hold,
    Land,
    Disarm,
    Failsafe_land,
    Idle,
    Takeoff
};

#define RAD2DEG(x) ((x)*180./M_PI)
//--------------------------------------------输入--------------------------------------------------
sensor_msgs::LaserScan Laser;                                   //激光雷达点云数据
geometry_msgs::PoseStamped pos_drone;                                  //无人机当前位置
float target_x;                                                 //期望位置_x
float target_y;                                                 //期望位置_y

int range_min;                                                //激光雷达探测范围 最小角度
int range_max;                                                //激光雷达探测范围 最大角度
float last_time = 0;
//--------------------------------------------算法相关--------------------------------------------------
float R_outside,R_inside;                                       //安全半径 [避障算法相关参数]
float p_R;                                                      //大圈比例参数
float p_r;                                                      //小圈比例参数

float distance_c,angle_c;                                       //最近障碍物距离 角度
float distance_cx,distance_cy;                                  //最近障碍物距离XY
float vel_collision[2];                                         //躲避障碍部分速度
float vel_collision_max;                                        //躲避障碍部分速度限幅

float p_xy;                                                     //追踪部分位置环P
float vel_track[2];                                             //追踪部分速度
float vel_track_max;                                            //追踪部分速度限幅
int flag_land;                                                  //降落标志位
//--------------------------------------------dyx-------------------------------------------------
float door_center_x[2];
float door_center_y[2];
bool reach_door_flag[2];
//--------------------------------------------dyx for 4x4demo-------------------------------------------------
float A_x,A_y;
float B_x,B_y;
float C_x,C_y;
bool reach_ABC_flag[3];
bool return_origin_flag[3];
//--------------------------------------------输出--------------------------------------------------
std_msgs::Bool flag_collision_avoidance;                       //是否进入避障模式标志位
float vel_sp_body[2];                                           //总速度
float vel_sp_max;                                               //总速度限幅
px4_command::command Command_now;                               //发送给position_control.cpp的命令
//>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>声 明 函 数<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<
void cal_min_distance();
float satfunc(float data, float Max);
void printf();                                                                       //打印函数
void printf_param();                                                                 //打印各项参数以供检查
void collision_avoidance(float target_x,float target_y);
void finddoorcentor(int i);
//>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>回 调 函 数<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<
//接收雷达的数据，并做相应处理,然后计算前后左右四向最小距离
void lidar_cb(const sensor_msgs::LaserScan::ConstPtr& scan)
{
    Laser = *scan;

    int count;    //count = 719
    //count = Laser.scan_time / Laser.time_increment; //dyx
    //cout<<count<<endl;
    count = Laser.ranges.size();

    //-179°到180°
    //cout << "Angle_range : "<< RAD2DEG(Laser.angle_min) << " to " << RAD2DEG(Laser.angle_max) <<endl;

    //剔除inf的情况
    //for(int i = 0; i <= count; i++)
    for(int i = 0; i < count; i++)
    {
        //判断是否为inf
        int a = isinf(Laser.ranges[i]);

        //如果为inf，则赋值上一角度的值
        if(a == 1)
        {
            if(i == 0)
            {
                //Laser.ranges[i] = Laser.ranges[count];
                Laser.ranges[i] = Laser.ranges[count-1];
            }
            else
            {
                Laser.ranges[i] = Laser.ranges[i-1];
            }
        }
    }

    //计算前后左右四向最小距离
    cal_min_distance();

}

void pos_cb(const geometry_msgs::PoseStamped::ConstPtr &msg)
{
    pos_drone = *msg;

}
//>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>主 函 数<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<
int main(int argc, char **argv)
{
    ros::init(argc, argv, "collision_avoidance");
    ros::NodeHandle nh("~");

    // 频率 [20Hz]
    ros::Rate rate(20.0);

    //【订阅】Lidar数据
    //ros::Subscriber lidar_sub = nh.subscribe<sensor_msgs::LaserScan>("/scan", 1000, lidar_cb);//dyx
    ros::Subscriber lidar_sub = nh.subscribe<sensor_msgs::LaserScan>("/lidar2Dscan", 1000, lidar_cb);//dyx

    //【订阅】无人机当前位置 坐标系 NED系
    //ros::Subscriber position_sub = nh.subscribe<geometry_msgs::Pose>("/drone/pos", 100, pos_cb);
    ros::Subscriber position_sub = nh.subscribe<geometry_msgs::PoseStamped>("/mavros/local_position/pose", 100, pos_cb);  //dyx

    // 【发布】发送给position_control.cpp的命令
    ros::Publisher command_pub = nh.advertise<px4_command::command>("/px4/command", 10);

    //读取参数表中的参数
    nh.param<float>("target_x", target_x, 1.0); //dyx
    nh.param<float>("target_y", target_y, 0.0); //dyx

    nh.param<float>("R_outside", R_outside, 2);
    nh.param<float>("R_inside", R_inside, 1);

    nh.param<float>("p_xy", p_xy, 0.5);

    nh.param<float>("vel_track_max", vel_track_max, 0.5);

    nh.param<float>("p_R", p_R, 0.0);
    nh.param<float>("p_r", p_r, 0.0);

    nh.param<float>("vel_collision_max", vel_collision_max, 0.0);
    nh.param<float>("vel_sp_max", vel_sp_max, 0.0);

    nh.param<int>("range_min", range_min, 0.0);
    nh.param<int>("range_max", range_max, 0.0);

    nh.param<float>("A_x", A_x, 0.0);
    nh.param<float>("A_y", A_y, 0.0);
    nh.param<float>("B_x", B_x, 0.0);
    nh.param<float>("B_y", B_y, 0.0);
    nh.param<float>("C_x", C_x, 0.0);
    nh.param<float>("C_y", C_y, 0.0);



    //打印现实检查参数
    printf_param();

    int check_flag;
    //输入1,继续，其他，退出程序
    cout << "Please check the parameter and setting，1 for go on， else for quit: "<<endl;
    cin >> check_flag;


    if(check_flag != 1)
    {
        return -1;
    }
    int mode_num;
    cout << "Which mdoe? 1 for door, 2 for 4x4demo, 3 for normal: "<<endl;
    cin >> mode_num;

    //初值
    vel_track[0]= 0;
    vel_track[1]= 0;

    vel_collision[0]= 0;
    vel_collision[1]= 0;

    vel_sp_body[0]= 0;
    vel_sp_body[1]= 0;

    //四向最小距离 初值
    flag_land = 0;

    //输出指令初始化
    int comid = 1;

//>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>Main Loop<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<
    while(ros::ok())
    {
        //回调一次 更新传感器状态
        //1. 更新雷达点云数据，存储在Laser中,并计算四向最小距离
        ros::spinOnce();
        /**************************dyx****************************************/
        if(mode_num==1)
        {
            if(!reach_door_flag[0]) finddoorcentor(0);
            else if(reach_door_flag[0]&&!reach_door_flag[1]) finddoorcentor(1);
            else if(reach_door_flag[0]&&reach_door_flag[1]) collision_avoidance(target_x,target_y);
        }
        if(mode_num==2)
        {
            if(!reach_ABC_flag[0])  //飞到A点，标记1，
            {
                collision_avoidance(A_x,A_y);
                float abs_distance;
                abs_distance = sqrt((pos_drone.pose.position.x - A_x) * (pos_drone.pose.position.x -A_x) + (pos_drone.pose.position.y - A_y) * (pos_drone.pose.position.y - A_y));
                //cout<<"abs_distance: "<<abs_distance<<endl;
                if(abs_distance < 0.3 )
                {
                    reach_ABC_flag[0]=true;
                }
            }
            else if(!return_origin_flag[0]) //返航，标记，
            {
                collision_avoidance(0,0);
                float abs_distance;
                abs_distance = sqrt((pos_drone.pose.position.x ) * (pos_drone.pose.position.x) + (pos_drone.pose.position.y ) * (pos_drone.pose.position.y ));
                if(abs_distance < 0.1 )
                {
                    return_origin_flag[0]=true;
                }
            }
            else if(!reach_ABC_flag[1])
            {
                collision_avoidance(B_x,B_y);
                float abs_distance;
                abs_distance = sqrt((pos_drone.pose.position.x - B_x) * (pos_drone.pose.position.x -B_x) + (pos_drone.pose.position.y - B_y) * (pos_drone.pose.position.y - B_y));
                //cout<<"abs_distance: "<<abs_distance<<endl;
                if(abs_distance < 0.3 )
                {
                    reach_ABC_flag[1]=true;
                }

            }
            else if(!return_origin_flag[1])  //返航，标记，
            {
                collision_avoidance(0,0);
                float abs_distance;
                abs_distance = sqrt((pos_drone.pose.position.x ) * (pos_drone.pose.position.x) + (pos_drone.pose.position.y ) * (pos_drone.pose.position.y ));
                if(abs_distance < 0.1 )
                {
                    return_origin_flag[1]=true;
                }

            }
            else if(!reach_ABC_flag[2])
            {
                collision_avoidance(C_x,C_y);
                float abs_distance;
                abs_distance = sqrt((pos_drone.pose.position.x - C_x) * (pos_drone.pose.position.x -C_x) + (pos_drone.pose.position.y - C_y) * (pos_drone.pose.position.y - C_y));
                //cout<<"abs_distance: "<<abs_distance<<endl;
                if(abs_distance < 0.3 )
                {
                    reach_ABC_flag[2]=true;
                }

            }
            else if(!return_origin_flag[2])  //返航，标记，
            {
                collision_avoidance(0,0);
                float abs_distance;
                abs_distance = sqrt((pos_drone.pose.position.x ) * (pos_drone.pose.position.x) + (pos_drone.pose.position.y ) * (pos_drone.pose.position.y ));
                if(abs_distance < 0.1 )
                {
                    flag_land=1;
                }
            }


        }
        if(mode_num==3)
        {
            collision_avoidance(target_x,target_y);
        }

        /**************************dyx****************************************/

        //5. 发布Command指令给position_controller.cpp
        Command_now.command = Move_Body;     //机体系下移动
        Command_now.comid = comid;
        comid++;
        Command_now.sub_mode = 2; // xy 速度控制模式 z 位置控制模式
        Command_now.vel_sp[0] =  vel_sp_body[0];
        Command_now.vel_sp[1] =  vel_sp_body[1];  //ENU frame
        Command_now.pos_sp[2] =  0;
        Command_now.yaw_sp = 0 ;
        if(mode_num!=2)
        {
            float abs_distance;
            abs_distance = sqrt((pos_drone.pose.position.x - target_x) * (pos_drone.pose.position.x - target_x) + (pos_drone.pose.position.y - target_y) * (pos_drone.pose.position.y - target_y));
            if(abs_distance < 0.3 || flag_land == 1)
            {
                Command_now.command = 3;     //Land
                flag_land = 1;
            }
        }
        else if(flag_land == 1) Command_now.command = 3;

        command_pub.publish(Command_now);

        //打印
        printf();

        rate.sleep();

    }

    return 0;

}


//计算前后左右四向最小距离
void cal_min_distance()
{

    distance_c = Laser.ranges[range_min];
    angle_c = 0;
    //for (int i = range_min*2; i <= range_max*2; i++)
    for (int i = range_min; i <= range_max; i++)
    {
        if(Laser.ranges[i] < distance_c)
        {
            distance_c = Laser.ranges[i];
            //angle_c = i/2;
            angle_c = i;
        }
    }
}


//饱和函数
float satfunc(float data, float Max)
{
    if(abs(data)>Max)
    {
        return ( data > 0 ) ? Max : -Max;
    }
    else
    {
        return data;
    }
}


void printf()
{

    cout <<">>>>>>>>>>>>>>>>>>>>>>>>>>>>>collision_avoidance<<<<<<<<<<<<<<<<<<<<<<<<<<<<<" <<endl;

    cout << "Minimun_distance : "<<endl;
    cout << "Distance : " << distance_c << " [m] "<<endl;
    cout << "Angle :    " << angle_c    << " [du] "<<endl;
    cout << "distance_cx :    " << distance_cx    << " [m] "<<endl;
    cout << "distance_cy :    " << distance_cy    << " [m] "<<endl;


    if(flag_collision_avoidance.data == true)
    {
        cout << "Collision avoidance Enabled "<<endl;
    }
    else
    {
        cout << "Collision avoidance Disabled "<<endl;
    }

    cout << "vel_track_x : " << vel_track[0] << " [m/s] "<<endl;
    cout << "vel_track_y : " << vel_track[1] << " [m/s] "<<endl;

    cout << "vel_collision_x : " << vel_collision[0] << " [m/s] "<<endl;
    cout << "vel_collision_y : " << vel_collision[1] << " [m/s] "<<endl;

    cout << "vel_sp_x : " << vel_sp_body[0] << " [m/s] "<<endl;
    cout << "vel_sp_y : " << vel_sp_body[1] << " [m/s] "<<endl;
    //ROS_WARN("CostMap Area: 16m2");
    double time_now = ros::Time::now().toSec();
    float interval = rand()/double(RAND_MAX)*0.3+1;
    //ROS_WARN("Navigation Serch cost time: %fs", interval);
    float search_speed = 16/interval;
    //ROS_WARN("Serch speed: %fm2/s", search_speed);
    last_time = time_now;
    

}

void printf_param()
{
    cout <<">>>>>>>>>>>>>>>>>>>>>>>>>>>>>> Parameter <<<<<<<<<<<<<<<<<<<<<<<<<<<" <<endl;
    cout << "target_x : "<< target_x << endl;
    cout << "target_y : "<< target_y << endl;

    cout << "R_outside : "<< R_outside << endl;
    cout << "R_inside : "<< R_inside << endl;

    cout << "p_xy : "<< p_xy << endl;
    cout << "vel_track_max : "<< vel_track_max << endl;

    cout << "p_R : "<< p_R << endl;
    cout << "p_r : "<< p_r << endl;

    cout << "vel_collision_max : "<< vel_collision_max << endl;

    cout << "vel_sp_max : "<< vel_sp_max << endl;
    cout << "range_min : "<< range_min << endl;
    cout << "range_max : "<< range_max << endl;
    cout << "A : "<< A_x<<" "<<A_y<< endl;
    cout << "B : "<< B_x<<" "<<B_y<< endl;
    cout << "C : "<< C_x<<" "<<C_y<< endl;

}
void collision_avoidance(float target_x,float target_y)
{


    //2. 根据最小距离判断：是否启用避障策略
    if (distance_c >= R_outside )
    {
        flag_collision_avoidance.data = false;
    }
    else
    {
        flag_collision_avoidance.data = true;
    }

    //3. 计算追踪速度
    vel_track[0] = p_xy * (target_x - pos_drone.pose.position.x);
    vel_track[1] = p_xy * (target_y - pos_drone.pose.position.y);

    //速度限幅
    for (int i = 0; i < 2; i++)
    {
        vel_track[i] = satfunc(vel_track[i],vel_track_max);
    }
    vel_collision[0]= 0;
    vel_collision[1]= 0;

    //4. 避障策略
    if(flag_collision_avoidance.data == true)
    {
        distance_cx = distance_c * cos(angle_c/180*3.1415926);
        distance_cy = distance_c * sin(angle_c/180*3.1415926);

        //distance_cx = - distance_cx;  //dyx

        float F_c;

        F_c = 0;

        if(distance_c > R_outside)
        {
            //对速度不做限制
            vel_collision[0] = vel_collision[0] + 0;
            vel_collision[1] = vel_collision[1] + 0;
            cout << " Forward Outside "<<endl;
        }

        //小幅度抑制移动速度
        if(distance_c > R_inside && distance_c <= R_outside)
        {
            F_c = p_R * (R_outside - distance_c);

        }

        //大幅度抑制移动速度
        if(distance_c <= R_inside )
        {
            F_c = p_R * (R_outside - R_inside) + p_r * (R_inside - distance_c);
        }

        if(distance_cx > 0)
        {
            vel_collision[0] = vel_collision[0] - F_c * distance_cx /distance_c;
        }else{
            vel_collision[0] = vel_collision[0] - F_c * distance_cx /distance_c;
        }

        if(distance_cy > 0)
        {
            vel_collision[1] = vel_collision[1] - F_c * distance_cy / distance_c;
        }else{
            vel_collision[1] = vel_collision[1] - F_c * distance_cy /distance_c;
        }


        //避障速度限幅
        for (int i = 0; i < 2; i++)
        {
            vel_collision[i] = satfunc(vel_collision[i],vel_collision_max);
        }
    }

    vel_sp_body[0] = vel_track[0] + vel_collision[0];
    //vel_sp_body[1] = vel_track[1] - vel_collision[1]; //dyx
    vel_sp_body[1] = vel_track[1] + vel_collision[1]; //dyx
    //vel_sp_body[0] = vel_track[0];
    //vel_sp_body[1] = vel_track[1];
    //vel_sp_body[0] = 0.1;
    //vel_sp_body[1] = 0;

    //找当前位置到目标点的xy差值，如果出现其中一个差值小，另一个差值大，
    //且过了一会还是保持这个差值就开始从差值入手。
    //比如，y方向接近0，但x还差很多，但x方向有障碍，这个时候按discx cy的大小，缓解y的难题。

    for (int i = 0; i < 2; i++)
    {
        vel_sp_body[i] = satfunc(vel_sp_body[i],vel_sp_max);
    }

}
void finddoorcentor(int i)
{
    //1.if no centor , findcentor set target
    //2.use collision_avoidance
    //3.judge whether reach target

    //1.
    float a,b,c;
    double l;
    cout<<"********************"<<endl;
    if(!door_center_x[i])
    {
        a=Laser.ranges[0];
        b=Laser.ranges[89];
        c=Laser.ranges[270];
        int theta1=atan(b/a)/3.1415926*180;
        int theta2=atan(c/a)/3.1415926*180;
        cout<<"theta1: "<<theta1<<endl;
        cout<<"theta2: "<<theta2<<endl;
        std::vector<int> door_angle;
        door_angle.reserve(theta1+theta2);
        for(int k=theta1;k>0;k--){
            float angle=k;
            l=a/cos(angle/180*3.1415926);
            float dl=abs(l-Laser.ranges[k]);
            if(dl>1) door_angle.push_back(k);
            //cout<<"k: "<<k<<" l: "<<l<<" Laser: "<<Laser.ranges[k]<<" dl: "<<dl<<endl;
        }
        for(int k=0;k<=theta2;k++){
            float angle=k;
            l=a/cos(angle/180*3.1415926);
            float dl=abs(l-Laser.ranges[359-k]);
            if(dl>1) door_angle.push_back(359-k);
            //cout<<"k: "<<359-k<<" l: "<<l<<" Laser: "<<Laser.ranges[359-k]<<" dl: "<<dl<<endl;
        }
        cout<<"door angle num: "<<door_angle.size()<<endl;
        cout<<"first :"<<door_angle.front()<<"last one: "<<door_angle.back()<<endl;
        int the1 = door_angle.front();
        int the2 = door_angle.back();
        float angle1,angle2;
        float x1,x2,y1,y2;
        x1=a;
        x2=a;
        if(the1>270)
        {
            angle1=359-the1;
            y1=-a*tan(angle1/180*3.1415926);
        }
        else
        {
            angle1=the1;
            y1=a*tan(angle1/180*3.1415926);
        }
        if(the2>270)
        {
            angle2=359-the2;
            y2=-a*tan(angle2/180*3.1415926);
        }
        else
        {
            angle2=the2;
            y2=a*tan(angle2/180*3.1415926);
        }


        cout<<"x1 y1: "<<x1<<" "<<y1<<endl;
        cout<<"x2 y2: "<<x2<<" "<<y2<<endl;

        door_center_x[i]=(x1+x2)/2+pos_drone.pose.position.x;
        door_center_y[i]=(y1+y2)/2+pos_drone.pose.position.y;
        cout<<"door position: "<<door_center_x[i]<<" "<<door_center_y[i]<<endl;
    }
    collision_avoidance(door_center_x[i]+0.5,door_center_y[i]);
    float abs_distance;
    abs_distance = sqrt((pos_drone.pose.position.x - door_center_x[i]-0.5) * (pos_drone.pose.position.x - door_center_x[i]-0.5) + (pos_drone.pose.position.y - door_center_y[i]) * (pos_drone.pose.position.y - door_center_y[i]));
    //cout<<"abs_distance: "<<abs_distance<<endl;
    cout<<"door position: "<<door_center_x[i]<<" "<<door_center_y[i]<<endl;
    if(abs_distance < 0.3 )
    {
        reach_door_flag[i]=true;
    }
}
